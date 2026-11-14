/*
 * test-sandbox-probe.c - A hostile probe inside a real box, and proof that
 * each protection is the one holding
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Behind CLAWT_TEST_INTEGRATION=1, because it runs bubblewrap; skipped
 * cleanly without it and when bwrap is not installed.
 *
 * Builds both boxes clawt-sandbox.c makes, with the real builders and a
 * real configuration -- the agent-CLI box through
 * clawt_agent_confine_build() and clawt_process_runtime_prepare(), the
 * exec box through clawt_computer_factory_create() and
 * clawt_computer_exec() -- runs tests/clawt-sandbox-probe.c inside each,
 * and expects every escape to fail.
 *
 * Then the mutation table.  One entry per protection the builder applies,
 * each taken out through clawt_sandbox_test_set_mutations() (or, for the
 * environment allowlist, which is not the sandbox's, by the harness
 * handing the box the daemon's whole environment).  Each entry names the
 * probe checks it must fail, per box, and the run has to fail *exactly*
 * those: a mutation that fails nothing is a test failure, because a check
 * that has never been shown to fail may not be able to (OpenMausBot's
 * 844139d5e: a loopback check that sat on the harness bridge's own port).
 *
 * What the harness holds constant, so that a check can fail only because
 * of the protection under test:
 *
 *   - The fixture lives in the build tree, not under /tmp, so the box's
 *     private /tmp is guarding one thing (the host /tmp marker) and not
 *     the whole fixture besides.
 *   - The listeners -- TCP on loopback, TCP on a LAN address of this
 *     machine, an abstract unix socket -- are opened here, on ephemeral
 *     ports nothing bridges into the box, and drained after every run so
 *     a full backlog cannot turn a reachable port into a timeout.
 *   - PATH is /usr/bin:/bin (plus bwrap's directory), so an AI CLI
 *     installed under the developer's home is not bound in and the box is
 *     the same on every machine.
 *   - The secret's value never reaches the spec; only its name does.
 *   - Every box gets the same config apart from the one flag.
 */

#define _GNU_SOURCE

#include <clawtilla.h>
#include "computer/clawt-sandbox-private.h"

#include <glib/gstdio.h>
#include <gio/gio.h>

#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "clawt-test-util.h"

#define PROBE_TIMEOUT_SECONDS (30)
#define AGENT_ID              ("probe")
#define OTHER_AGENT_ID        ("other")

/* Outcomes as the probe reports them. */
typedef enum {
	OUTCOME_HELD,
	OUTCOME_LEAKED,
	OUTCOME_SKIPPED
} Outcome;

/* Which of the two boxes. */
typedef enum {
	BOX_AGENT,
	BOX_EXEC
} BoxKind;

/*
 * One protection, the flag that takes it out, and the checks that must
 * fail without it -- per box, because the agent box has the network by
 * design and only it is started from a terminal.  A comma list; an empty
 * string means "this box does not have the protection", which is allowed
 * only while the other box shows it failing something.
 */
typedef struct {
	const gchar *name;
	guint        flags;          /* ClawtSandboxMutation */
	gboolean     inherit_env;    /* the harness's, not the sandbox's */
	const gchar *agent_fails;
	const gchar *exec_fails;
} Mutation;

static const Mutation mutations[] = {
	{ "share-tmp", CLAWT_SANDBOX_MUTATE_SHARE_TMP, FALSE,
	  "host_tmp",
	  "host_tmp" },
	{ "share-home", CLAWT_SANDBOX_MUTATE_SHARE_HOME, FALSE,
	  "daemon_state,other_workspace,home_ssh,home_documents",
	  "daemon_state,other_workspace,home_ssh,home_documents" },
	{ "widen-root", CLAWT_SANDBOX_MUTATE_WIDEN_ROOT, FALSE,
	  "other_workspace",
	  "other_workspace" },
	{ "share-net", CLAWT_SANDBOX_MUTATE_SHARE_NET, FALSE,
	  "",
	  "loopback,lan" },
	{ "share-abstract", CLAWT_SANDBOX_MUTATE_SHARE_ABSTRACT, FALSE,
	  "abstract_unix",
	  "" },
	{ "share-pid", CLAWT_SANDBOX_MUTATE_SHARE_PID, FALSE,
	  "host_pids",
	  "host_pids" },
	{ "share-session", CLAWT_SANDBOX_MUTATE_SHARE_SESSION, FALSE,
	  "controlling_tty",
	  "" },
	{ "ro-writable", CLAWT_SANDBOX_MUTATE_RO_WRITABLE, FALSE,
	  "ro_bind_write",
	  "ro_bind_write" },
	{ "skip-denials", CLAWT_SANDBOX_MUTATE_SKIP_DENIALS, FALSE,
	  "denied_read",
	  "denied_read" },
	{ "agent-env-wins", CLAWT_SANDBOX_MUTATE_AGENT_ENV_WINS, FALSE,
	  "env_override",
	  "env_override" },
	{ "skip-link-check", CLAWT_SANDBOX_MUTATE_SKIP_LINK_CHECK, FALSE,
	  "hardlink_write,hardlink_deny",
	  "hardlink_write,hardlink_deny" },
	{ "inherit-env", 0, TRUE,
	  "env_secret",
	  "env_secret" }
};

/* The checks only the hard-link run makes; the main run skips them. */
static const gchar *const hardlink_checks[] = {
	"hardlink_write", "hardlink_deny", NULL
};

typedef struct {
	gchar *base;          /* in the build tree, canonical */
	gchar *home;          /* $HOME for this process and every box */
	gchar *state;         /* daemon.state_dir */
	gchar *workspace;     /* this agent's */
	gchar *other;         /* another agent's */
	gchar *shared;        /* allow_paths / confine_paths */
	gchar *ro_share;      /* a `mode: ro` mount */
	gchar *ro_file;       /* a single-file `mode: ro` mount, hard-linked */
	gchar *deny_file;     /* a denied file, hard-linked */
	gchar *evil;          /* where env: points TMPDIR, HOME, XDG_* */
	gchar *host_tmp_dir;  /* under the real /tmp */
	gchar *host_tmp;      /* the marker in it */
	gchar *probe;         /* the probe, copied into the workspace */
	gchar *secret_name;
	gchar *loopback;      /* "127.0.0.1:port" */
	gchar *lan;           /* "a.b.c.d:port", or NULL */
	gchar *abstract;      /* an abstract socket's name */
	gint   listeners[3];
	GSubprocess *canary;
	guint64 canary_start;
	guint64 daemon_start;
} Fixture;

static Fixture fixture;
static gboolean integration_ready;
static const gchar *skip_reason;

/* ── Small helpers ───────────────────────────────────────────────── */

static void
write_file(
	const gchar *path,
	const gchar *text
){
	g_autofree gchar *parent = g_path_get_dirname(path);

	g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

static guint64
process_start(
	gint pid
){
	g_autofree gchar *path = g_strdup_printf("/proc/%d/stat", pid);
	g_autofree gchar *text = NULL;
	g_auto(GStrv) fields = NULL;
	const gchar *after;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;

	after = strrchr(text, ')');

	if (after == NULL || after[1] == '\0')
		return 0;

	fields = g_strsplit(after + 2, " ", -1);

	if (g_strv_length(fields) < 20)
		return 0;

	return g_ascii_strtoull(fields[19], NULL, 10);
}

/*
 * The canary dies with this process.  A failed GTest case aborts the
 * binary, so no teardown runs, and a `sleep 600` left behind holding
 * the caller's pipe kept `make test-integration` waiting on it.
 */
static void
die_with_test(
	gpointer data
){
	prctl(PR_SET_PDEATHSIG, SIGKILL);
}

/* ── Listeners ───────────────────────────────────────────────────── */

/*
 * A TCP listener on @address, port 0.  Returns the fd and writes
 * "address:port" to @out.  Nothing accepts: the kernel completes the
 * handshake into the backlog, which is what "reachable" means here, and
 * drain_listeners() empties it between runs.
 */
static gint
listen_inet(
	const gchar  *address,
	gchar       **out
){
	struct sockaddr_in bound;
	socklen_t length;
	gint fd;

	fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	g_assert_cmpint(fd, >=, 0);

	memset(&bound, 0, sizeof(bound));
	bound.sin_family = AF_INET;
	bound.sin_port = 0;
	g_assert_cmpint(inet_pton(AF_INET, address, &bound.sin_addr), ==, 1);
	g_assert_cmpint(bind(fd, (struct sockaddr *)&bound,
	                     (socklen_t)sizeof(bound)), ==, 0);
	g_assert_cmpint(listen(fd, 64), ==, 0);

	length = (socklen_t)sizeof(bound);
	g_assert_cmpint(getsockname(fd, (struct sockaddr *)&bound, &length),
	                ==, 0);

	*out = g_strdup_printf("%s:%u", address, (guint)ntohs(bound.sin_port));

	return fd;
}

static gint
listen_abstract(
	const gchar *name
){
	struct sockaddr_un bound;
	gsize length = strlen(name);
	gint fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	g_assert_cmpint(fd, >=, 0);

	memset(&bound, 0, sizeof(bound));
	bound.sun_family = AF_UNIX;
	memcpy(bound.sun_path + 1, name, length);

	g_assert_cmpint(bind(fd, (struct sockaddr *)&bound,
	                     (socklen_t)(G_STRUCT_OFFSET(struct sockaddr_un,
	                                                 sun_path) + 1 + length)),
	                ==, 0);
	g_assert_cmpint(listen(fd, 64), ==, 0);

	return fd;
}

/*
 * An IPv4 address of this machine that is not loopback, or NULL.  From
 * inside a box with its own network namespace it is unroutable; from one
 * that shares the host's, it is this machine -- so a listener on it
 * stands for the LAN without the test needing a LAN.
 */
static gchar *
find_lan_address(
	void
){
	struct ifaddrs *list;
	struct ifaddrs *entry;
	gchar *found;

	found = NULL;

	if (getifaddrs(&list) != 0)
		return NULL;

	for (entry = list; entry != NULL && found == NULL; entry = entry->ifa_next) {
		gchar text[INET_ADDRSTRLEN];
		const struct sockaddr_in *address;

		if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET)
			continue;

		if ((entry->ifa_flags & IFF_UP) == 0 ||
		    (entry->ifa_flags & IFF_LOOPBACK) != 0)
			continue;

		address = (const struct sockaddr_in *)entry->ifa_addr;

		if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != NULL)
			found = g_strdup(text);
	}

	freeifaddrs(list);

	return found;
}

/* Accepts and closes whatever the last probe left in each backlog. */
static void
drain_listeners(
	void
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(fixture.listeners); i++) {
		gint client;

		if (fixture.listeners[i] < 0)
			continue;

		while ((client = accept4(fixture.listeners[i], NULL, NULL,
		                         SOCK_CLOEXEC)) >= 0)
			close(client);
	}
}

/* ── The fixture ─────────────────────────────────────────────────── */

/*
 * Laid out the way the defaults lay out a real fleet: the state
 * directory is ~/.clawtilla and workspaces are under it, so the private
 * home is what hides the daemon's files from the agent box and the
 * absence of any home bind is what hides them from the exec box.
 *
 * Runs before g_test_init(), because $HOME has to be the fixture's
 * before anything asks GLib for the home directory, which it caches.
 */
static void
fixture_create(
	const gchar *bwrap
){
	g_autofree gchar *template = NULL;
	g_autofree gchar *made = NULL;
	g_autofree gchar *outdir = NULL;
	g_autofree gchar *bwrap_dir = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *probe_source = NULL;
	g_autofree gchar *lan_address = NULL;
	const gchar *tmp;
	gchar random_part[17];
	guint i;

	outdir = g_path_is_absolute(BUILD_OUTDIR)
		? g_strdup(BUILD_OUTDIR)
		: g_build_filename(CLAWT_TEST_SRCDIR, BUILD_OUTDIR, NULL);
	template = g_build_filename(outdir, "sandbox-probe-XXXXXX", NULL);
	made = g_mkdtemp(g_strdup(template));
	g_assert_nonnull(made);

	fixture.base = clawt_canonicalize_missing(made);
	fixture.home = g_build_filename(fixture.base, "home", NULL);
	fixture.state = g_build_filename(fixture.home, ".clawtilla", NULL);
	fixture.workspace = g_build_filename(fixture.state, "agents", AGENT_ID,
	                                     NULL);
	fixture.other = g_build_filename(fixture.state, "agents",
	                                 OTHER_AGENT_ID, NULL);
	fixture.shared = g_build_filename(fixture.home, "shared", NULL);
	fixture.ro_share = g_build_filename(fixture.home, "ro-share", NULL);
	fixture.ro_file = g_build_filename(fixture.home, "ro-file.txt", NULL);
	fixture.deny_file = g_build_filename(fixture.shared, "secret.txt", NULL);
	fixture.evil = g_build_filename(fixture.base, "evil", NULL);

	/* What the boxes must not show. */
	path = g_build_filename(fixture.home, ".ssh", "id_ed25519", NULL);
	write_file(path, "fixture ssh key\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.home, "Documents", "notes.org", NULL);
	write_file(path, "fixture document\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.state, "tcp-token", NULL);
	write_file(path, "fixture daemon token\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.other, "MEMORY.org", NULL);
	write_file(path, "another agent's memory\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.shared, "private", "key", NULL);
	write_file(path, "denied directory contents\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.shared, "token", NULL);
	write_file(path, "denied file contents\n");
	g_clear_pointer(&path, g_free);
	path = g_build_filename(fixture.ro_share, "readme", NULL);
	write_file(path, "read-only mount\n");
	g_clear_pointer(&path, g_free);
	write_file(fixture.ro_file, "read-only file\n");
	write_file(fixture.deny_file, "denied, and linked\n");
	g_assert_cmpint(g_mkdir_with_parents(fixture.workspace, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(fixture.evil, 0700), ==, 0);

	/*
	 * The host /tmp marker goes under the real /tmp: that is what the
	 * box's private /tmp hides.  Under `make test` TMPDIR is a private
	 * directory inside /tmp, which the litter check watches, so it goes
	 * there when it can.
	 */
	tmp = g_get_tmp_dir();

	if (clawt_path_is_within(tmp, "/tmp")) {
		fixture.host_tmp_dir = g_dir_make_tmp("clawt-probe-tmp-XXXXXX",
		                                      NULL);
	} else {
		fixture.host_tmp_dir = g_mkdtemp(
			g_strdup("/tmp/clawt-probe-tmp-XXXXXX"));
	}

	g_assert_nonnull(fixture.host_tmp_dir);
	fixture.host_tmp = g_build_filename(fixture.host_tmp_dir, "marker",
	                                    NULL);
	write_file(fixture.host_tmp, "host tmp\n");

	/* The probe, inside the workspace so neither box needs a bind for it. */
	probe_source = g_build_filename(outdir, "tests", "clawt-sandbox-probe",
	                                NULL);
	fixture.probe = g_build_filename(fixture.workspace,
	                                 "clawt-sandbox-probe", NULL);
	{
		g_autoptr(GFile) from = g_file_new_for_path(probe_source);
		g_autoptr(GFile) to = g_file_new_for_path(fixture.probe);
		g_autoptr(GError) error = NULL;

		g_file_copy(from, to, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
		            &error);
		g_assert_no_error(error);
		g_assert_cmpint(g_chmod(fixture.probe, 0755), ==, 0);
	}

	/* A secret in the daemon's environment that the allowlist must stop. */
	for (i = 0; i < 16; i++)
		random_part[i] = (gchar)('A' + g_random_int_range(0, 26));
	random_part[16] = '\0';
	fixture.secret_name = g_strdup_printf("CLAWT_PROBE_SECRET_%s",
	                                      random_part);
	g_setenv(fixture.secret_name, "fixture-secret-value", TRUE);

	/* Listeners, on ports nothing bridges into any box. */
	fixture.listeners[0] = listen_inet("127.0.0.1", &fixture.loopback);
	lan_address = find_lan_address();
	fixture.listeners[1] = (lan_address != NULL)
		? listen_inet(lan_address, &fixture.lan) : -1;
	fixture.abstract = g_strdup_printf("clawt-probe-%d-%s", (gint)getpid(),
	                                   random_part);
	fixture.listeners[2] = listen_abstract(fixture.abstract);

	/*
	 * A host process the box must not see.  Started with a fixed
	 * environment, so its own environ holds nothing of the daemon's.
	 */
	{
		g_autoptr(GSubprocessLauncher) launcher =
			g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
			                          G_SUBPROCESS_FLAGS_STDERR_SILENCE);
		g_autoptr(GError) error = NULL;
		const gchar *argv[] = { "/usr/bin/sleep", "600", NULL };
		const gchar *env[] = { "PATH=/usr/bin", NULL };

		g_subprocess_launcher_set_environ(launcher, (gchar **)env);
		g_subprocess_launcher_set_child_setup(launcher, die_with_test,
		                                      NULL, NULL);
		fixture.canary = g_subprocess_launcher_spawnv(launcher, argv, &error);
		g_assert_no_error(error);
		g_usleep(50 * 1000);
		fixture.canary_start = process_start(
			(gint)g_ascii_strtoll(g_subprocess_get_identifier(fixture.canary),
			                      NULL, 10));
	}

	fixture.daemon_start = process_start((gint)getpid());

	/*
	 * $HOME is the fixture's, before GLib caches one; PATH is the
	 * system's, so a CLI under the real home is not bound in.
	 */
	g_setenv("HOME", fixture.home, TRUE);
	g_unsetenv("XDG_CONFIG_HOME");
	g_unsetenv("XDG_DATA_HOME");
	g_unsetenv("XDG_CACHE_HOME");
	g_unsetenv("XDG_STATE_HOME");
	bwrap_dir = g_path_get_dirname(bwrap);
	{
		g_autofree gchar *system_path =
			g_strdup_printf("%s:/usr/bin:/bin", bwrap_dir);

		g_setenv("PATH", system_path, TRUE);
	}
}

static void
fixture_destroy(
	gboolean keep
){
	guint i;

	for (i = 0; i < G_N_ELEMENTS(fixture.listeners); i++) {
		if (fixture.listeners[i] >= 0)
			close(fixture.listeners[i]);
	}

	if (fixture.canary != NULL) {
		g_subprocess_force_exit(fixture.canary);
		g_subprocess_wait(fixture.canary, NULL, NULL);
		g_clear_object(&fixture.canary);
	}

	/*
	 * Left behind on a failed run, as evidence: the fixture is under the
	 * build tree and `make clean` takes it.
	 */
	if (!keep) {
		clawt_test_remove_tree(fixture.base);
		clawt_test_remove_tree(fixture.host_tmp_dir);
	}
}

/* The hard links exist only for the run that tests them. */
static void
make_hardlinks(
	gboolean present
){
	g_autofree gchar *ro_link = g_build_filename(fixture.workspace,
	                                             "ro-link", NULL);
	g_autofree gchar *deny_link = g_build_filename(fixture.workspace,
	                                               "deny-link", NULL);

	g_unlink(ro_link);
	g_unlink(deny_link);

	if (!present)
		return;

	g_assert_cmpint(link(fixture.ro_file, ro_link), ==, 0);
	g_assert_cmpint(link(fixture.deny_file, deny_link), ==, 0);
}

/* ── Configuration ───────────────────────────────────────────────── */

/*
 * One agent with both boxes: `runtime.confine: bwrap` for its own
 * process, `computer.host.confine: bwrap` with the network off for
 * exec.  @hardlinks adds the single-file read-only mount and the denied
 * file that each have a second name in the workspace.
 */
static ClawtConfig *
load_config_full(
	gboolean hardlinks,
	gboolean exec_network
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *extra_mount = NULL;
	g_autofree gchar *extra_deny = NULL;
	ClawtConfig *config;

	extra_mount = hardlinks
		? g_strdup_printf("        - source: \"%s\"\n"
		                  "          target: \"/mnt/ro-file.txt\"\n"
		                  "          mode: ro\n", fixture.ro_file)
		: g_strdup("");
	extra_deny = hardlinks
		? g_strdup_printf(", \"%s\"", fixture.deny_file)
		: g_strdup("");

	yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"defaults:\n"
		"  workspace_root: \"%s/agents\"\n"
		"agents:\n"
		"  - id: %s\n"
		"    workspace: \"%s\"\n"
		"    runtime:\n"
		"      type: process\n"
		"      confine: bwrap\n"
		"      confine_paths: [\"%s\"]\n"
		"    env:\n"
		"      TMPDIR: \"%s\"\n"
		"      HOME: \"%s\"\n"
		"      XDG_CONFIG_HOME: \"%s\"\n"
		"    computer:\n"
		"      type: host\n"
		"      exchange: false\n"
		"      default_mounts: false\n"
		"      mounts:\n"
		"        - source: \"%s\"\n"
		"          target: \"/mnt/ro-share\"\n"
		"          mode: ro\n"
		"%s"
		"      host:\n"
		"        confirm_host_control: true\n"
		"        confine: bwrap\n"
		"        allow_network: %s\n"
		"        allow_paths: [\"%s\"]\n"
		"        deny_paths: [\"%s/private\", \"%s/token\"%s]\n",
		fixture.state, fixture.state, AGENT_ID, fixture.workspace,
		fixture.shared, fixture.evil, fixture.evil, fixture.evil,
		fixture.ro_share, extra_mount, exec_network ? "true" : "false",
		fixture.shared, fixture.shared, fixture.shared, extra_deny);

	config = clawt_config_load_from_string(yaml, &error);
	g_assert_no_error(error);
	g_assert_nonnull(config);

	return config;
}

static ClawtConfig *
load_config(
	gboolean hardlinks
){
	return load_config_full(hardlinks, FALSE);
}

/*
 * The spec the probe reads: where everything is, never what it holds.
 */
static gchar *
write_spec(
	BoxKind       box,
	gboolean      hardlinks,
	const gchar  *const *env_allow
){
	g_autoptr(GKeyFile) spec = g_key_file_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *daemon_state = NULL;
	g_autofree gchar *other = NULL;
	g_autofree gchar *ssh = NULL;
	g_autofree gchar *documents = NULL;
	g_autofree gchar *ro_file = NULL;
	g_autofree gchar *private_key = NULL;
	g_autofree gchar *token = NULL;
	const gchar *group = "probe";

	path = g_build_filename(fixture.workspace,
	                        box == BOX_AGENT ? "spec-agent.ini"
	                                         : "spec-exec.ini", NULL);
	daemon_state = g_build_filename(fixture.state, "tcp-token", NULL);
	other = g_build_filename(fixture.other, "MEMORY.org", NULL);
	ssh = g_build_filename(fixture.home, ".ssh", "id_ed25519", NULL);
	documents = g_build_filename(fixture.home, "Documents", "notes.org",
	                             NULL);
	ro_file = g_build_filename(fixture.ro_share, "readme", NULL);
	private_key = g_build_filename(fixture.shared, "private", "key", NULL);
	token = g_build_filename(fixture.shared, "token", NULL);

	g_key_file_set_string(spec, group, "box",
	                      box == BOX_AGENT ? "agent" : "exec");
	g_key_file_set_string(spec, group, "workspace", fixture.workspace);

	if (hardlinks) {
		g_autofree gchar *ro_link = g_build_filename(fixture.workspace,
		                                             "ro-link", NULL);
		g_autofree gchar *deny_link = g_build_filename(fixture.workspace,
		                                               "deny-link", NULL);
		GStatBuf st;

		g_assert_cmpint(g_stat(fixture.deny_file, &st), ==, 0);
		g_key_file_set_string(spec, group, "hardlink_write", ro_link);
		g_key_file_set_string(spec, group, "hardlink_write_ro",
		                      fixture.ro_file);
		g_key_file_set_string(spec, group, "hardlink_deny", deny_link);
		g_key_file_set_uint64(spec, group, "hardlink_deny_inode",
		                      (guint64)st.st_ino);
	} else {
		const gchar *denied[3];

		denied[0] = private_key;
		denied[1] = token;
		denied[2] = NULL;

		g_key_file_set_string(spec, group, "daemon_state", daemon_state);
		g_key_file_set_string(spec, group, "other_workspace", other);
		g_key_file_set_string(spec, group, "home_ssh", ssh);
		g_key_file_set_string(spec, group, "home_documents", documents);
		g_key_file_set_string(spec, group, "host_tmp", fixture.host_tmp);
		g_key_file_set_string_list(spec, group, "denied", denied, 2);
		g_key_file_set_string(spec, group, "ro_dir", fixture.ro_share);
		g_key_file_set_string(spec, group, "ro_file", ro_file);
		g_key_file_set_integer(spec, group, "daemon_pid", (gint)getpid());
		g_key_file_set_uint64(spec, group, "daemon_start",
		                      fixture.daemon_start);
		g_key_file_set_integer(spec, group, "canary_pid",
		                       (gint)g_ascii_strtoll(
		                           g_subprocess_get_identifier(fixture.canary),
		                           NULL, 10));
		g_key_file_set_uint64(spec, group, "canary_start",
		                      fixture.canary_start);
		g_key_file_set_string(spec, group, "network",
		                      box == BOX_AGENT ? "allowed" : "denied");
		g_key_file_set_string(spec, group, "loopback", fixture.loopback);

		if (fixture.lan != NULL)
			g_key_file_set_string(spec, group, "lan", fixture.lan);

		g_key_file_set_string(spec, group, "abstract", fixture.abstract);
		g_key_file_set_boolean(spec, group, "abstract_scoped",
		                       clawt_sandbox_can_scope_abstract_sockets());
		g_key_file_set_string(spec, group, "env_evil", fixture.evil);
		g_key_file_set_string(spec, group, "env_secret",
		                      fixture.secret_name);
		g_key_file_set_string_list(spec, group, "env_allow", env_allow,
		                           g_strv_length((gchar **)env_allow));
		g_key_file_set_boolean(spec, group, "tty", box == BOX_AGENT);
	}

	g_assert_true(g_key_file_save_to_file(spec, path, &error));
	g_assert_no_error(error);

	return g_steal_pointer(&path);
}

/*
 * The names a box may legitimately carry: what the allowlist passes plus
 * the agent's own env:.  Computed from the same function the runtime
 * uses -- if that ever passes the secret, the separate name check in the
 * probe still catches it.
 */
static GStrv
allowed_names(
	GHashTable *agent_env
){
	g_auto(GStrv) environment = clawt_build_child_environment(agent_env);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	guint i;

	for (i = 0; environment[i] != NULL; i++) {
		const gchar *equals = strchr(environment[i], '=');

		if (equals != NULL)
			g_ptr_array_add(names, g_strndup(environment[i],
			                                 (gsize)(equals - environment[i])));
	}

	g_ptr_array_add(names, NULL);

	return (GStrv)g_ptr_array_free(g_steal_pointer(&names), FALSE);
}

/* The daemon's whole environment, as an env: block would hand it over. */
static GHashTable *
whole_environment(
	void
){
	g_auto(GStrv) environment = g_get_environ();
	GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                          g_free, g_free);
	guint i;

	for (i = 0; environment[i] != NULL; i++) {
		const gchar *equals = strchr(environment[i], '=');

		if (equals == NULL)
			continue;

		g_hash_table_replace(table,
		                     g_strndup(environment[i],
		                               (gsize)(equals - environment[i])),
		                     g_strdup(equals + 1));
	}

	return table;
}

/* ── Reading the report ──────────────────────────────────────────── */

/*
 * TAP from the probe into name -> Outcome.  FALSE when there was nothing
 * that looked like a report, which is a box that did not start.
 */
static gboolean
parse_tap(
	const gchar *text,
	GHashTable  *outcomes
){
	g_auto(GStrv) lines = NULL;
	gboolean any = FALSE;
	guint i;

	if (text == NULL)
		return FALSE;

	lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines[i] != NULL; i++) {
		const gchar *line = lines[i];
		const gchar *rest;
		Outcome outcome;
		gsize length;

		if (g_str_has_prefix(line, "not ok ")) {
			outcome = OUTCOME_LEAKED;
			rest = line + strlen("not ok ");
		} else if (g_str_has_prefix(line, "ok ")) {
			outcome = OUTCOME_HELD;
			rest = line + strlen("ok ");
		} else {
			continue;
		}

		rest = strstr(rest, " - ");

		if (rest == NULL)
			continue;

		rest += 3;

		if (strstr(rest, " # SKIP ") != NULL)
			outcome = OUTCOME_SKIPPED;

		length = strcspn(rest, ": #");
		g_hash_table_replace(outcomes, g_strndup(rest, length),
		                     GINT_TO_POINTER(outcome));
		any = TRUE;
	}

	return any;
}

/* ── Running a box ───────────────────────────────────────────────── */

typedef struct {
	gboolean  done;
	gchar    *stdout_text;
	gchar    *stderr_text;
	GError   *error;
} SpawnWait;

static void
on_communicated(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	SpawnWait *wait = (SpawnWait *)data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
	                                     &wait->stdout_text,
	                                     &wait->stderr_text, &wait->error);
	wait->done = TRUE;
}

static gboolean
on_deadline(
	gpointer data
){
	g_subprocess_force_exit(G_SUBPROCESS(data));
	return G_SOURCE_REMOVE;
}

typedef struct {
	const gchar  *slave;
	ClawtSandbox *sandbox;
} TerminalSetup;

/*
 * Child setup for the agent box: a session of its own whose controlling
 * terminal is a fresh pty, standing in for the terminal a daemon started
 * by hand has -- --new-session is what must take it away again -- and
 * then exactly what the runtime's own child setup does for a confined
 * agent, clawt_sandbox_enter().
 */
static void
take_terminal(
	gpointer data
){
	TerminalSetup *setup = (TerminalSetup *)data;
	gint fd;

	setsid();
	fd = open(setup->slave, O_RDWR);

	if (fd >= 0)
		ioctl(fd, TIOCSCTTY, 0);

	clawt_sandbox_enter(setup->sandbox);
}

/*
 * The agent-CLI box: the runtime's own argv and environment, from
 * clawt_process_runtime_prepare(), with the probe in libreclaw's place.
 * Spawned here rather than by clawt_agent_runtime_start() only so the
 * child can be given a terminal.  Returns FALSE with @refused set when
 * the runtime refused to build the box.
 */
static gboolean
run_agent_box(
	ClawtAgentConfig  *agent_config,
	guint              flags,
	GHashTable        *env,
	const gchar       *spec,
	GHashTable        *outcomes,
	gchar            **refused,
	gchar            **diagnostics
){
	g_autoptr(ClawtProcessRuntime) runtime = NULL;
	g_autoptr(ClawtSandbox) sandbox = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GMainContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) environment = NULL;
	g_autofree gchar *slave = NULL;
	SpawnWait wait = { FALSE, NULL, NULL, NULL };
	TerminalSetup setup;
	GSource *deadline;
	gint master;
	gboolean parsed;

	runtime = clawt_process_runtime_new(agent_config, spec);
	clawt_process_runtime_set_binary(runtime, fixture.probe);
	clawt_process_runtime_set_environment(runtime, env);

	sandbox = clawt_agent_confine_build(agent_config, fixture.workspace,
	                                    NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	clawt_sandbox_test_set_mutations(sandbox, flags);
	clawt_process_runtime_set_sandbox(runtime, sandbox);

	if (!clawt_process_runtime_prepare(runtime, &argv, &environment, &error)) {
		*refused = g_strdup(error->message);
		return FALSE;
	}

	master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	g_assert_cmpint(master, >=, 0);
	g_assert_cmpint(grantpt(master), ==, 0);
	g_assert_cmpint(unlockpt(master), ==, 0);
	slave = g_strdup(ptsname(master));
	g_assert_nonnull(slave);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_environ(launcher, environment);
	g_subprocess_launcher_set_cwd(launcher, fixture.workspace);
	setup.slave = slave;
	setup.sandbox = sandbox;
	g_subprocess_launcher_set_child_setup(launcher, take_terminal, &setup,
	                                      NULL);
	child = g_subprocess_launcher_spawnv(launcher,
	                                     (const gchar * const *)argv, &error);
	g_assert_no_error(error);

	/* A box that never answers fails the run rather than hanging it. */
	context = g_main_context_new();
	g_main_context_push_thread_default(context);
	g_subprocess_communicate_utf8_async(child, NULL, NULL, on_communicated,
	                                    &wait);
	deadline = g_timeout_source_new_seconds(PROBE_TIMEOUT_SECONDS);
	g_source_set_callback(deadline, on_deadline, child, NULL);
	g_source_attach(deadline, context);

	while (!wait.done)
		g_main_context_iteration(context, TRUE);

	g_source_destroy(deadline);
	g_source_unref(deadline);
	g_main_context_pop_thread_default(context);
	close(master);

	parsed = parse_tap(wait.stdout_text, outcomes);
	*diagnostics = g_strdup_printf("agent box stdout:\n%s\nstderr:\n%s\n%s",
	                               wait.stdout_text != NULL
	                                   ? wait.stdout_text : "",
	                               wait.stderr_text != NULL
	                                   ? wait.stderr_text : "",
	                               wait.error != NULL ? wait.error->message
	                                                  : "");
	g_free(wait.stdout_text);
	g_free(wait.stderr_text);
	g_clear_error(&wait.error);

	g_assert_true(parsed);

	return TRUE;
}

/*
 * The exec box: clawt_computer_exec() on the host computer the factory
 * builds, exactly the path clawtilla_computer_exec takes.
 */
static gboolean
run_exec_box(
	ClawtAgentConfig  *agent_config,
	guint              flags,
	GHashTable        *env,
	const gchar       *spec,
	GHashTable        *outcomes,
	gchar            **refused,
	gchar            **diagnostics
){
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(GError) error = NULL;
	ClawtExecResult *result;
	ClawtSandbox *sandbox;
	const gchar *argv[4];
	gboolean parsed;

	computer = clawt_computer_factory_create(agent_config, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(CLAWT_IS_HOST_COMPUTER(computer));
	g_assert_true(clawt_computer_provision(computer, &error));
	g_assert_no_error(error);

	sandbox = clawt_host_computer_get_sandbox(CLAWT_HOST_COMPUTER(computer));
	clawt_sandbox_test_set_mutations(sandbox, flags);
	clawt_host_computer_set_environment(CLAWT_HOST_COMPUTER(computer), env);

	argv[0] = fixture.probe;
	argv[1] = "-c";
	argv[2] = spec;
	argv[3] = NULL;

	result = clawt_computer_exec(computer, argv, fixture.workspace,
	                             PROBE_TIMEOUT_SECONDS, NULL, &error);

	if (result == NULL) {
		*refused = g_strdup(error != NULL ? error->message : "refused");
		return FALSE;
	}

	parsed = parse_tap(clawt_exec_result_get_stdout(result), outcomes);
	*diagnostics = g_strdup_printf("exec box exit %d stdout:\n%s\nstderr:\n%s",
	                               clawt_exec_result_get_exit_status(result),
	                               clawt_exec_result_get_stdout(result),
	                               clawt_exec_result_get_stderr(result));
	clawt_exec_result_free(result);

	g_assert_true(parsed);

	return TRUE;
}

/*
 * Both runs of one box under one mutation, merged into @outcomes: the
 * main run for every check, and the hard-link run for the two checks
 * that need a second name in the workspace.  The hard-link run is
 * refused before anything spawns when clawt_sandbox_check_links() is
 * doing its job, and a refusal holds both of those checks.
 */
static void
run_box(
	BoxKind      box,
	guint        flags,
	gboolean     inherit_env,
	GHashTable  *outcomes,
	GString     *log
){
	gboolean hardlinks;

	for (hardlinks = FALSE; hardlinks <= TRUE; hardlinks++) {
		g_autoptr(ClawtConfig) config = NULL;
		g_autoptr(GHashTable) env = NULL;
		g_autoptr(GHashTable) found = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
		g_auto(GStrv) allow = NULL;
		g_autofree gchar *spec = NULL;
		g_autofree gchar *refused = NULL;
		g_autofree gchar *diagnostics = NULL;
		ClawtAgentConfig *agent_config;
		gboolean ran;
		guint i;

		make_hardlinks(hardlinks);
		config = load_config(hardlinks);
		agent_config = clawt_config_get_agent(config, AGENT_ID);
		g_assert_nonnull(agent_config);

		env = clawt_agent_config_get_env(agent_config);
		allow = allowed_names(env);

		if (inherit_env) {
			g_clear_pointer(&env, g_hash_table_unref);
			env = whole_environment();
		}

		spec = write_spec(box, hardlinks, (const gchar * const *)allow);
		drain_listeners();

		ran = (box == BOX_AGENT)
			? run_agent_box(agent_config, flags, env, spec, found,
			                &refused, &diagnostics)
			: run_exec_box(agent_config, flags, env, spec, found,
			               &refused, &diagnostics);

		if (!ran) {
			/*
			 * Only the hard-link run may be refused, and only by the
			 * link check: any other refusal is a box that did not start.
			 */
			if (!hardlinks || strstr(refused, "hard link") == NULL) {
				g_test_fail_printf("%s box refused to start: %s",
				                   box == BOX_AGENT ? "agent" : "exec",
				                   refused);
				continue;
			}

			for (i = 0; hardlink_checks[i] != NULL; i++)
				g_hash_table_replace(outcomes, g_strdup(hardlink_checks[i]),
				                     GINT_TO_POINTER(OUTCOME_HELD));
			continue;
		}

		g_string_append_printf(log, "%s\n", diagnostics);

		if (GPOINTER_TO_INT(g_hash_table_lookup(found,
		                                        "own_workspace_writable")) !=
		        OUTCOME_HELD ||
		    !g_hash_table_contains(found, "own_workspace_writable")) {
			g_test_fail_printf("the %s box could not write its own "
			                   "workspace, so nothing it reported means "
			                   "anything:\n%s",
			                   box == BOX_AGENT ? "agent" : "exec",
			                   diagnostics);
			continue;
		}

		/* The main run owns every check but the hard-link pair. */
		{
			GHashTableIter iter;
			gpointer key;
			gpointer value;

			g_hash_table_iter_init(&iter, found);

			while (g_hash_table_iter_next(&iter, &key, &value)) {
				gboolean pair = g_strv_contains(hardlink_checks, key);

				if (pair == hardlinks)
					g_hash_table_replace(outcomes, g_strdup(key), value);
			}
		}
	}

	make_hardlinks(FALSE);
}

/* The checks that leaked, sorted, as one comma list. */
static gchar *
leaked_list(
	GHashTable *outcomes
){
	g_autoptr(GPtrArray) names = g_ptr_array_new();
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	GString *out;
	guint i;

	g_hash_table_iter_init(&iter, outcomes);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		if (GPOINTER_TO_INT(value) == OUTCOME_LEAKED)
			g_ptr_array_add(names, key);
	}

	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	out = g_string_new(NULL);

	for (i = 0; i < names->len; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? "," : "",
		                       (const gchar *)g_ptr_array_index(names, i));

	return g_string_free(out, FALSE);
}

/*
 * The table's expectation for one box, less any check the baseline run
 * skipped -- a machine with no LAN address has no `lan` check to fail --
 * and sorted the way leaked_list() sorts.
 */
static gchar *
expected_list(
	const gchar *table,
	GHashTable  *baseline
){
	g_auto(GStrv) names = g_strsplit(table, ",", -1);
	g_autoptr(GPtrArray) kept = g_ptr_array_new();
	GString *out;
	guint i;

	for (i = 0; names[i] != NULL; i++) {
		if (*names[i] == '\0')
			continue;

		if (GPOINTER_TO_INT(g_hash_table_lookup(baseline, names[i])) ==
		        OUTCOME_SKIPPED)
			continue;

		g_ptr_array_add(kept, names[i]);
	}

	g_ptr_array_sort_values(kept, (GCompareFunc)g_strcmp0);
	out = g_string_new(NULL);

	for (i = 0; i < kept->len; i++)
		g_string_append_printf(out, "%s%s", i > 0 ? "," : "",
		                       (const gchar *)g_ptr_array_index(kept, i));

	return g_string_free(out, FALSE);
}

/* ── The tests ───────────────────────────────────────────────────── */

static gboolean
integration_or_skip(
	void
){
	if (!integration_ready) {
		g_test_skip(skip_reason);
		return FALSE;
	}

	return TRUE;
}

/*
 * The real boxes: every escape fails, in both.  Skips are allowed only
 * where the spec says why (the agent box has the network by design, the
 * exec box is not started from a terminal, a machine may have no LAN
 * address); every other check must have run and held.
 */
static void
test_real_boxes_hold(
	void
){
	BoxKind box;

	if (!integration_or_skip())
		return;

	for (box = BOX_AGENT; box <= BOX_EXEC; box++) {
		g_autoptr(GHashTable) outcomes = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
		g_autoptr(GString) log = g_string_new(NULL);
		g_autofree gchar *leaked = NULL;

		run_box(box, 0, FALSE, outcomes, log);
		leaked = leaked_list(outcomes);
		g_test_message("%s", log->str);

		if (*leaked != '\0')
			g_test_fail_printf("the %s box leaked: %s\n%s",
			                   box == BOX_AGENT ? "agent" : "exec",
			                   leaked, log->str);

		/* Every check that is not allowed to skip, ran. */
		{
			static const gchar *const always[] = {
				"daemon_state", "other_workspace", "home_ssh",
				"home_documents", "host_tmp", "denied_read",
				"ro_bind_write", "hardlink_write", "hardlink_deny",
				"host_pids", "env_override", "env_secret", NULL
			};
			guint i;

			for (i = 0; always[i] != NULL; i++) {
				gpointer value;

				if (!g_hash_table_lookup_extended(outcomes, always[i], NULL,
				                                  &value) ||
				    GPOINTER_TO_INT(value) != OUTCOME_HELD)
					g_test_fail_printf("the %s box did not run %s",
					                   box == BOX_AGENT ? "agent" : "exec",
					                   always[i]);
			}
		}

		if (box == BOX_AGENT &&
		    GPOINTER_TO_INT(g_hash_table_lookup(outcomes,
		                                        "controlling_tty")) !=
		        OUTCOME_HELD)
			g_test_fail_printf("the agent box did not run controlling_tty");

		if (box == BOX_AGENT && clawt_sandbox_can_scope_abstract_sockets() &&
		    GPOINTER_TO_INT(g_hash_table_lookup(outcomes,
		                                        "abstract_unix")) !=
		        OUTCOME_HELD)
			g_test_fail_printf("the agent box did not hold abstract_unix");

		if (box == BOX_EXEC) {
			if (GPOINTER_TO_INT(g_hash_table_lookup(outcomes, "loopback")) !=
			        OUTCOME_HELD ||
			    GPOINTER_TO_INT(g_hash_table_lookup(outcomes,
			                                        "abstract_unix")) !=
			        OUTCOME_HELD)
				g_test_fail_printf("the exec box did not run its network "
				                   "checks");
		}
	}
}

/*
 * The mutation table: each protection out, one at a time, must fail
 * exactly the checks the table names for each box -- and at least one
 * somewhere, or it is a protection no check can see.
 */
static void
test_mutations_fail_exactly_their_checks(
	void
){
	g_autoptr(GHashTable) baseline_agent = NULL;
	g_autoptr(GHashTable) baseline_exec = NULL;
	g_autoptr(GString) table = g_string_new(NULL);
	guint m;

	if (!integration_or_skip())
		return;

	baseline_agent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                       NULL);
	baseline_exec = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      NULL);
	{
		g_autoptr(GString) log = g_string_new(NULL);

		run_box(BOX_AGENT, 0, FALSE, baseline_agent, log);
		run_box(BOX_EXEC, 0, FALSE, baseline_exec, log);
	}

	for (m = 0; m < G_N_ELEMENTS(mutations); m++) {
		const Mutation *mutation = &mutations[m];
		g_autoptr(GHashTable) agent = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
		g_autoptr(GHashTable) exec = g_hash_table_new_full(
			g_str_hash, g_str_equal, g_free, NULL);
		g_autoptr(GString) log = g_string_new(NULL);
		g_autofree gchar *agent_leaked = NULL;
		g_autofree gchar *exec_leaked = NULL;
		g_autofree gchar *agent_expected = NULL;
		g_autofree gchar *exec_expected = NULL;

		run_box(BOX_AGENT, mutation->flags, mutation->inherit_env, agent,
		        log);
		run_box(BOX_EXEC, mutation->flags, mutation->inherit_env, exec,
		        log);

		agent_leaked = leaked_list(agent);
		exec_leaked = leaked_list(exec);
		agent_expected = expected_list(mutation->agent_fails,
		                               baseline_agent);
		exec_expected = expected_list(mutation->exec_fails, baseline_exec);

		g_string_append_printf(table, "%-16s agent: %-50s exec: %s\n",
		                       mutation->name,
		                       *agent_leaked != '\0' ? agent_leaked : "-",
		                       *exec_leaked != '\0' ? exec_leaked : "-");

		/*
		 * A row whose every check the baseline skipped is a protection
		 * this machine does not have (no Landlock scopes, say), not one
		 * that failed to show: said, and passed over.
		 */
		if (*agent_expected == '\0' && *exec_expected == '\0' &&
		    *agent_leaked == '\0' && *exec_leaked == '\0') {
			g_test_message("mutation %s: not applicable on this machine; "
			               "every check it guards was skipped",
			               mutation->name);
			continue;
		}

		if (*agent_leaked == '\0' && *exec_leaked == '\0') {
			g_test_fail_printf("mutation %s failed no probe check in "
			                   "either box: a protection nothing can be "
			                   "shown to depend on\n%s", mutation->name,
			                   log->str);
			continue;
		}

		if (g_strcmp0(agent_leaked, agent_expected) != 0)
			g_test_fail_printf("mutation %s, agent box: failed [%s], the "
			                   "table says [%s]\n%s", mutation->name,
			                   agent_leaked, agent_expected, log->str);

		if (g_strcmp0(exec_leaked, exec_expected) != 0)
			g_test_fail_printf("mutation %s, exec box: failed [%s], the "
			                   "table says [%s]\n%s", mutation->name,
			                   exec_leaked, exec_expected, log->str);
	}

	g_test_message("mutation table:\n%s", table->str);

	if (g_getenv("CLAWT_TEST_PROBE_TABLE") != NULL)
		g_printerr("%s", table->str);
}

/*
 * The agent box once more, started the way the daemon starts it --
 * clawt_agent_runtime_start(), its own launcher and its own child
 * setup -- because the other tests spawn prepare()'s argv themselves to
 * hand it a terminal.  This is what shows the runtime's child setup
 * really applies clawt_sandbox_enter(): abstract_unix has to hold here
 * too.  The probe writes its report to a file named in the spec, since
 * the runtime reads the child's stdout as a log.
 */
static void
test_runtime_start_path(
	void
){
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(ClawtProcessRuntime) runtime = NULL;
	g_autoptr(ClawtSandbox) sandbox = NULL;
	g_autoptr(GHashTable) env = NULL;
	g_autoptr(GHashTable) outcomes = NULL;
	g_autoptr(GKeyFile) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) allow = NULL;
	g_autofree gchar *spec_path = NULL;
	g_autofree gchar *report = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *leaked = NULL;
	ClawtAgentConfig *agent_config;
	guint waited;

	if (!integration_or_skip())
		return;

	config = load_config(FALSE);
	agent_config = clawt_config_get_agent(config, AGENT_ID);
	env = clawt_agent_config_get_env(agent_config);
	allow = allowed_names(env);
	spec_path = write_spec(BOX_AGENT, FALSE, (const gchar * const *)allow);

	/* Started without a terminal, and told so; and told where to write. */
	report = g_build_filename(fixture.workspace, "report.tap", NULL);
	g_unlink(report);
	spec = g_key_file_new();
	g_assert_true(g_key_file_load_from_file(spec, spec_path,
	                                        G_KEY_FILE_NONE, &error));
	g_key_file_set_boolean(spec, "probe", "tty", FALSE);
	g_key_file_set_string(spec, "probe", "report", report);
	g_assert_true(g_key_file_save_to_file(spec, spec_path, &error));
	g_assert_no_error(error);

	runtime = clawt_process_runtime_new(agent_config, spec_path);
	clawt_process_runtime_set_binary(runtime, fixture.probe);
	clawt_process_runtime_set_environment(runtime, env);
	sandbox = clawt_agent_confine_build(agent_config, fixture.workspace,
	                                    NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	clawt_process_runtime_set_sandbox(runtime, sandbox);

	drain_listeners();
	g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
	                                        &error));
	g_assert_no_error(error);

	/* Bounded, so a box that never starts fails rather than hangs. */
	for (waited = 0; waited < PROBE_TIMEOUT_SECONDS * 10 &&
	                 !g_file_get_contents(report, &text, NULL, NULL);
	     waited++)
		g_usleep(100 * 1000);

	clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));
	g_assert_nonnull(text);
	g_test_message("%s", text);

	outcomes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_assert_true(parse_tap(text, outcomes));
	leaked = leaked_list(outcomes);
	g_assert_cmpstr(leaked, ==, "");
	g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(
	                    outcomes, "own_workspace_writable")), ==,
	                OUTCOME_HELD);

	if (clawt_sandbox_can_scope_abstract_sockets())
		g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(
		                    outcomes, "abstract_unix")), ==, OUTCOME_HELD);
}

/*
 * An exec box with `allow_network: true` -- the default for a host
 * computer -- keeps the host's network, so only the Landlock scope from
 * the host computer's own child setup stands between it and the host's
 * abstract sockets.  The mutation table cannot show that (its exec box is
 * offline, and the network namespace guards the same check), so this
 * does, through clawt_computer_exec().
 */
static void
test_networked_exec_box(
	void
){
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GHashTable) env = NULL;
	g_autoptr(GHashTable) outcomes = NULL;
	g_autoptr(GKeyFile) spec = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) allow = NULL;
	g_autofree gchar *spec_path = NULL;
	g_autofree gchar *refused = NULL;
	g_autofree gchar *diagnostics = NULL;
	g_autofree gchar *leaked = NULL;
	ClawtAgentConfig *agent_config;

	if (!integration_or_skip())
		return;

	if (!clawt_sandbox_can_scope_abstract_sockets()) {
		g_test_skip("this kernel has no Landlock scopes (ABI 6)");
		return;
	}

	config = load_config_full(FALSE, TRUE);
	agent_config = clawt_config_get_agent(config, AGENT_ID);
	env = clawt_agent_config_get_env(agent_config);
	allow = allowed_names(env);
	spec_path = write_spec(BOX_EXEC, FALSE, (const gchar * const *)allow);

	spec = g_key_file_new();
	g_assert_true(g_key_file_load_from_file(spec, spec_path,
	                                        G_KEY_FILE_NONE, &error));
	g_key_file_set_string(spec, "probe", "network", "allowed");
	g_assert_true(g_key_file_save_to_file(spec, spec_path, &error));
	g_assert_no_error(error);

	outcomes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	drain_listeners();
	g_assert_true(run_exec_box(agent_config, 0, env, spec_path, outcomes,
	                           &refused, &diagnostics));
	g_test_message("%s", diagnostics);

	leaked = leaked_list(outcomes);
	g_assert_cmpstr(leaked, ==, "");
	g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(
	                    outcomes, "abstract_unix")), ==, OUTCOME_HELD);
	g_assert_cmpint(GPOINTER_TO_INT(g_hash_table_lookup(
	                    outcomes, "loopback")), ==, OUTCOME_SKIPPED);
}

int
main(
	int   argc,
	char *argv[]
){
	g_autofree gchar *bwrap = NULL;
	gint status;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(fixture.listeners); i++)
		fixture.listeners[i] = -1;

	/*
	 * Decided before g_test_init(), because the fixture has to own $HOME
	 * before GLib caches one.
	 */
	bwrap = g_find_program_in_path("bwrap");

	if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
		skip_reason = "set CLAWT_TEST_INTEGRATION=1 to run the probe in a "
		              "real bwrap box";
	} else if (bwrap == NULL) {
		skip_reason = "bubblewrap is not installed";
	} else {
		fixture_create(bwrap);
		integration_ready = TRUE;
	}

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/sandbox-probe/real-boxes-hold", test_real_boxes_hold);
	g_test_add_func("/sandbox-probe/mutations-fail-exactly-their-checks",
	                test_mutations_fail_exactly_their_checks);
	g_test_add_func("/sandbox-probe/runtime-start-path",
	                test_runtime_start_path);
	g_test_add_func("/sandbox-probe/networked-exec-box",
	                test_networked_exec_box);

	status = g_test_run();

	if (integration_ready)
		fixture_destroy(status != 0);

	return status;
}
