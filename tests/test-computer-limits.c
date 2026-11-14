/*
 * test-computer-limits.c - Container limits, lazy start, and waking a machine
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic throughout. The podman and distrobox command lines are
 * asserted as data, never run; the lazy start is driven through a fake
 * backend whose start is slow enough for concurrent first uses to
 * overlap; and the wake command is a real process -- /bin/sh -- because
 * what is being tested is what happens to processes: that one runs per
 * wave, and that a timeout leaves none of them behind.
 */

#include <clawtilla.h>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "clawt-test-util.h"

/* ── Sizes and validation ────────────────────────────────────────── */

/*
 * Every spelling podman reads, in powers of 1024, and the ones it does
 * not -- above all the fraction, which is what people actually type.
 */
static void
test_size_parse(
	void
){
	static const struct {
		const gchar *text;
		gint64       bytes;
	} good[] = {
		{ "512",   512 },
		{ "512b",  512 },
		{ "4k",    (gint64)4 * 1024 },
		{ "512m",  (gint64)512 * 1024 * 1024 },
		{ "512M",  (gint64)512 * 1024 * 1024 },
		{ "4g",    (gint64)4 * 1024 * 1024 * 1024 },
		{ "4gb",   (gint64)4 * 1024 * 1024 * 1024 },
		{ "4GB",   (gint64)4 * 1024 * 1024 * 1024 }
	};
	static const gchar *const bad[] = {
		"", "g", "1.5g", "4t", "-4g", " 4g", "4 g", "0x10", "4gbb",
		"99999999999999999999", NULL
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		g_autoptr(GError) error = NULL;
		gint64 bytes = 0;

		g_assert_true(clawt_container_size_parse(good[i].text, &bytes,
		                                         &error));
		g_assert_no_error(error);
		g_assert_cmpint(bytes, ==, good[i].bytes);
	}

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_size_parse(bad[i], NULL, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	}

	/* The fraction's refusal names the spelling that works. */
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_size_parse("1.5g", NULL, &error));
		g_assert_nonnull(strstr(error->message, "1536m"));
	}
}

static ClawtContainerLimits *
limits_of(
	gint64       pids,
	const gchar *memory,
	const gchar *swap
){
	ClawtContainerLimits *limits;

	limits = clawt_container_limits_new();
	clawt_container_limits_set_pids_limit(limits, pids);
	clawt_container_limits_set_memory(limits, memory);
	clawt_container_limits_set_memory_swap(limits, swap);

	return limits;
}

/*
 * Each refusal names the key, because it is given where the value
 * arrives -- `agent set`, a client's form -- and somebody has to know
 * which field to fix.
 */
static void
test_limits_validate(
	void
){
	static const struct {
		gint64       pids;
		const gchar *memory;
		const gchar *swap;
		const gchar *names;   /* NULL when the limits are valid */
	} cases[] = {
		{ 0,    NULL,   NULL,   NULL },
		{ 2048, "4g",   "8g",   NULL },
		{ -1,   "512m", "-1",   NULL },
		{ 100,  "4g",   "4g",   NULL },
		{ -2,   NULL,   NULL,   "pids_limit" },
		{ 0,    "4m",   NULL,   "6m" },
		{ 0,    "lots", NULL,   "computer.container.memory" },
		{ 0,    NULL,   "8g",   "needs computer.container.memory" },
		{ 0,    "4g",   "2g",   "less than memory" },
		{ 0,    "4g",   "1.5g", "memory_swap" }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++) {
		g_autoptr(ClawtContainerLimits) limits =
			limits_of(cases[i].pids, cases[i].memory, cases[i].swap);
		g_autoptr(GError) error = NULL;
		gboolean ok;

		ok = clawt_container_limits_validate(limits, &error);

		if (cases[i].names == NULL) {
			g_assert_no_error(error);
			g_assert_true(ok);
		} else {
			g_assert_false(ok);
			g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
			g_assert_nonnull(strstr(error->message, cases[i].names));
		}
	}

	/* Blank is unset, so a cleared text field is not a value. */
	{
		g_autoptr(ClawtContainerLimits) limits = limits_of(0, "  ", "");

		g_assert_true(clawt_container_limits_is_empty(limits));
		g_assert_null(clawt_container_limits_get_memory(limits));
	}
}

/*
 * The order a client sends memory and its swap total in, one agent.set at
 * a time: memory first unless that would pass through a pair the daemon
 * refuses.
 */
static void
test_limits_swap_first(
	void
){
	/* Setting both from nothing, and lowering both: memory first. */
	g_assert_false(clawt_container_limits_swap_first(NULL, NULL, "4g", "6g"));
	g_assert_false(clawt_container_limits_swap_first("8g", "10g", "4g", "6g"));

	/* Raising memory past the stored total: the total first. */
	g_assert_true(clawt_container_limits_swap_first("4g", "6g", "8g", "10g"));

	/* Clearing memory under a stored total: the total first. */
	g_assert_true(clawt_container_limits_swap_first("4g", "6g", "", ""));
}

/* ── What reaches podman and distrobox ───────────────────────────── */

static void
assert_strv(
	const gchar * const *have,
	const gchar * const *want
){
	g_autofree gchar *have_line = clawt_command_line_join(have);
	g_autofree gchar *want_line = clawt_command_line_join(want);

	g_assert_cmpstr(have_line, ==, want_line);
	g_assert_cmpuint(g_strv_length((GStrv)have), ==,
	                 g_strv_length((GStrv)want));
}

/*
 * The podman options: one complete option per word, so that each can be
 * handed through distrobox whole; nothing at all when nothing is set.
 */
static void
test_limits_podman_create_args(
	void
){
	static const gchar *const want[] = {
		"--pids-limit=2048", "--memory=4g", "--memory-swap=8g", NULL
	};
	g_autoptr(ClawtContainerLimits) limits = limits_of(2048, "4G", "8g");
	g_autoptr(ClawtContainerLimits) none = clawt_container_limits_new();
	g_auto(GStrv) args = clawt_container_limits_create_args(limits);
	g_auto(GStrv) empty = clawt_container_limits_create_args(none);

	assert_strv((const gchar * const *)args, want);
	g_assert_cmpuint(g_strv_length(empty), ==, 0);
}

/*
 * distrobox splits its flags string with g_shell_parse_argv and passes
 * every word as its own --additional-flags. Parsed back the same way
 * here, each limit has to be exactly one word -- "--memory 4g" as two
 * would hand podman "4g" as the image -- and the operator's own flags
 * have to survive in front of them.
 */
static void
test_limits_distrobox_flags(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_of(512, "2g", NULL);
	g_autoptr(ClawtContainerLimits) none = clawt_container_limits_new();
	g_autofree gchar *flags = NULL;
	g_autofree gchar *only_user = NULL;
	g_autofree gchar *nothing = NULL;
	g_auto(GStrv) words = NULL;
	gint count = 0;

	flags = clawt_distrobox_computer_build_flags("--cap-add=SYS_PTRACE",
	                                             limits);
	g_assert_true(g_shell_parse_argv(flags, &count, &words, NULL));
	g_assert_cmpint(count, ==, 3);
	g_assert_cmpstr(words[0], ==, "--cap-add=SYS_PTRACE");
	g_assert_cmpstr(words[1], ==, "--pids-limit=512");
	g_assert_cmpstr(words[2], ==, "--memory=2g");

	only_user = clawt_distrobox_computer_build_flags("--net=host", none);
	g_assert_cmpstr(only_user, ==, "--net=host");

	nothing = clawt_distrobox_computer_build_flags(NULL, NULL);
	g_assert_null(nothing);
}

/*
 * The podman that owns the machine: a URI as --url (a remote podman over
 * ssh among them), a bare word as a named connection, nothing for the
 * local one -- which is what a distrobox always is.
 */
static void
test_limits_podman_prefix_and_inspect(
	void
){
	static const gchar *const remote[] = {
		"podman", "--url", "ssh://me@box/run/podman.sock", NULL
	};
	static const gchar *const named[] = {
		"podman", "--connection", "homelab", NULL
	};
	static const gchar *const local[] = { "podman", NULL };
	static const gchar *const inspect_want[] = {
		"podman", "container", "inspect", "--format",
		CLAWT_CONTAINER_LIMITS_INSPECT_FORMAT, "clawt-chief", NULL
	};
	g_auto(GStrv) a = clawt_container_limits_podman_prefix(
		"podman", "ssh://me@box/run/podman.sock");
	g_auto(GStrv) b = clawt_container_limits_podman_prefix("podman",
	                                                       "homelab");
	g_auto(GStrv) c = clawt_container_limits_podman_prefix("podman", NULL);
	g_auto(GStrv) inspect = clawt_container_limits_inspect_argv(
		"podman", NULL, "clawt-chief");

	assert_strv((const gchar * const *)a, remote);
	assert_strv((const gchar * const *)b, named);
	assert_strv((const gchar * const *)c, local);
	assert_strv((const gchar * const *)inspect, inspect_want);
}

/*
 * A changed limit is applied with `podman update`, and only what changed
 * is sent: the existing machine is updated in place, never rebuilt.
 */
static void
test_limits_update_on_change(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_of(100, "512m", "1g");

	/* podman's defaults: 2048 processes, no memory limit. */
	{
		static const gchar *const want[] = {
			"podman", "--url", "unix:///run/user/1000/podman/podman.sock",
			"update", "--pids-limit=100", "--memory=512m",
			"--memory-swap=1g", "clawt-chief", NULL
		};
		g_auto(GStrv) options =
			clawt_container_limits_update_args(limits, "2048 0 0\n");
		g_auto(GStrv) argv = clawt_container_limits_update_argv(
			"podman", "unix:///run/user/1000/podman/podman.sock",
			"clawt-chief", (const gchar * const *)options);

		assert_strv((const gchar * const *)argv, want);
	}

	/* Exactly what it has already: no update at all. */
	{
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "100 536870912 1073741824");
		g_auto(GStrv) argv = clawt_container_limits_update_argv(
			"podman", NULL, "clawt-chief", (const gchar * const *)options);

		g_assert_cmpuint(g_strv_length(options), ==, 0);
		g_assert_null(argv);
	}

	/*
	 * Only the process limit moved: only it is sent. The swap total is
	 * re-sent with memory, never without it -- podman checks the pair.
	 */
	{
		static const gchar *const want[] = { "--pids-limit=100", NULL };
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "4096 536870912 1073741824");

		assert_strv((const gchar * const *)options, want);
	}

	{
		static const gchar *const want[] = {
			"--memory=512m", "--memory-swap=1g", NULL
		};
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "100 268435456 1073741824");

		assert_strv((const gchar * const *)options, want);
	}

	/*
	 * Unlimited is reported as 0 or -1 depending on podman; both match a
	 * configured -1. And an inspect that could not be read applies every
	 * limit that is set rather than guessing.
	 */
	{
		g_autoptr(ClawtContainerLimits) open = limits_of(-1, "1g", "-1");
		g_auto(GStrv) same = clawt_container_limits_update_args(
			open, "-1 1073741824 0");
		g_auto(GStrv) unknown = clawt_container_limits_update_args(
			open, "garbage");

		g_assert_cmpuint(g_strv_length(same), ==, 0);
		g_assert_cmpuint(g_strv_length(unknown), ==, 3);
	}

	/* A limit left unset is never touched, whatever podman has. */
	{
		g_autoptr(ClawtContainerLimits) memory_only =
			limits_of(0, "1g", NULL);
		g_auto(GStrv) options = clawt_container_limits_update_args(
			memory_only, "64 1073741824 2147483648");

		g_assert_cmpuint(g_strv_length(options), ==, 0);
	}
}

/*
 * From the configuration to both backends: the same three keys reach a
 * container and a distrobox, a refused value stops the computer being
 * built (so the agent is told at once, naming the key), and the limits
 * are what the backend then holds.
 */
static void
test_limits_reach_both_backends(
	void
){
	static const gchar *const types[] = { "container", "distrobox" };
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(types); i++) {
		g_autofree gchar *yaml = g_strdup_printf(
			"agents:\n"
			"  - id: chief\n"
			"    computer:\n"
			"      type: %s\n"
			"      container:\n"
			"        pids_limit: 300\n"
			"        memory: 2g\n"
			"        memory_swap: 3g\n", types[i]);
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtConfig) config =
			clawt_config_load_from_string(yaml, &error);
		g_autoptr(ClawtComputer) computer = NULL;
		g_autoptr(ClawtContainerLimits) read = NULL;
		g_auto(GStrv) args = NULL;

		g_assert_no_error(error);

		read = clawt_computer_factory_limits(
			clawt_config_get_agent(config, "chief"));
		args = clawt_container_limits_create_args(read);
		g_assert_cmpuint(g_strv_length(args), ==, 3);

		computer = clawt_computer_factory_create(
			clawt_config_get_agent(config, "chief"), NULL, bridge, &error);
		g_assert_no_error(error);
		g_assert_nonnull(computer);

		if (CLAWT_IS_CONTAINER_COMPUTER(computer)) {
			const ClawtContainerLimits *held =
				clawt_container_computer_get_limits(
					CLAWT_CONTAINER_COMPUTER(computer));

			g_assert_nonnull(held);
			g_assert_cmpint(clawt_container_limits_get_pids_limit(held),
			                ==, 300);
			g_assert_cmpstr(clawt_container_limits_get_memory(held), ==,
			                "2g");
		} else {
			g_assert_true(CLAWT_IS_DISTROBOX_COMPUTER(computer));
		}
	}

	/* A pair podman would refuse is refused before anything is built. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
			"agents:\n"
			"  - id: chief\n"
			"    computer:\n"
			"      type: container\n"
			"      container:\n"
			"        memory: 4g\n"
			"        memory_swap: 1g\n", &error);
		g_autoptr(ClawtComputer) computer = NULL;

		g_assert_no_error(error);
		computer = clawt_computer_factory_create(
			clawt_config_get_agent(config, "chief"), NULL, bridge, &error);
		g_assert_null(computer);
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
		g_assert_nonnull(strstr(error->message, "memory_swap"));
	}
}

/*
 * The real thing, behind CLAWT_TEST_INTEGRATION: a container created
 * without limits gets them in place, a changed limit is updated in
 * place, and podman reports what was asked for.
 */
static void
test_limits_podman_integration(
	void
){
	g_autofree gchar *name = NULL;
	g_autofree gchar *created = NULL;
	g_autofree gchar *inspected = NULL;
	g_autoptr(GError) error = NULL;
	gint status = -1;

	if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL ||
	    g_find_program_in_path("podman") == NULL) {
		g_test_skip("needs CLAWT_TEST_INTEGRATION=1 and podman");
		return;
	}

	name = g_strdup_printf("clawt-limits-test-%d", (gint)getpid());

	{
		const gchar *create[] = {
			"podman", "create", "--name", NULL,
			"registry.fedoraproject.org/fedora:45", "sleep", "infinity",
			NULL
		};

		create[3] = name;
		g_assert_true(clawt_run_argv_bounded(create, 300, &status,
		                                     &created, NULL, &error));
		g_assert_no_error(error);

		if (status != 0) {
			g_test_skip("podman could not create a container here");
			return;
		}
	}

	{
		g_autoptr(ClawtContainerLimits) limits =
			limits_of(128, "256m", "512m");
		gboolean changed = FALSE;

		g_assert_true(clawt_container_limits_apply(limits, NULL, name,
		                                           &changed, &error));
		g_assert_no_error(error);
		g_assert_true(changed);

		/* Again, unchanged: nothing to update. */
		g_assert_true(clawt_container_limits_apply(limits, NULL, name,
		                                           &changed, &error));
		g_assert_false(changed);

		clawt_container_limits_set_pids_limit(limits, 64);
		g_assert_true(clawt_container_limits_apply(limits, NULL, name,
		                                           &changed, &error));
		g_assert_true(changed);
	}

	{
		g_auto(GStrv) inspect =
			clawt_container_limits_inspect_argv("podman", NULL, name);

		g_assert_true(clawt_run_argv_bounded(
			(const gchar * const *)inspect, 60, &status, &inspected, NULL,
			&error));
		g_assert_cmpint(status, ==, 0);
		g_assert_cmpstr(g_strstrip(inspected), ==, "64 268435456 536870912");
	}

	{
		const gchar *remove[] = { "podman", "rm", "-f", "-t", "0", NULL,
		                          NULL };

		remove[5] = name;
		(void)clawt_run_argv_bounded(remove, 120, &status, NULL, NULL,
		                             NULL);
	}
}

/* ── Lazy start ──────────────────────────────────────────────────── */

/*
 * A backend whose start takes long enough for first uses to overlap,
 * and counts how many times it actually ran.
 */
#define TEST_TYPE_SLOW_COMPUTER (test_slow_computer_get_type())
G_DECLARE_FINAL_TYPE(TestSlowComputer, test_slow_computer, TEST,
                     SLOW_COMPUTER, ClawtComputer)

struct _TestSlowComputer {
	ClawtComputer  parent_instance;

	gint           starts;     /* atomic */
	gint           execs;      /* atomic */
	gboolean       fail;
	guint          start_ms;
};

G_DEFINE_FINAL_TYPE(TestSlowComputer, test_slow_computer,
                    CLAWT_TYPE_COMPUTER)

static gboolean
slow_start(
	ClawtComputer  *computer,
	GError        **error
){
	TestSlowComputer *self;

	self = TEST_SLOW_COMPUTER(computer);
	g_atomic_int_inc(&self->starts);
	g_usleep((gulong)self->start_ms * 1000);

	if (self->fail) {
		clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
		                         "no image");
		g_set_error_literal(error, CLAWT_ERROR,
		                    CLAWT_ERROR_COMPUTER_PROVISION, "no image");
		return FALSE;
	}

	clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

	return TRUE;
}

static gboolean
slow_stop(
	ClawtComputer  *computer,
	GError        **error
){
	(void)error;

	clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

	return TRUE;
}

static ClawtExecResult *
slow_exec(
	ClawtComputer        *computer,
	const gchar * const  *argv,
	const gchar          *working_dir,
	guint                 timeout_seconds,
	GCancellable         *cancellable,
	GError              **error
){
	TestSlowComputer *self;

	(void)working_dir;
	(void)timeout_seconds;
	(void)cancellable;

	self = TEST_SLOW_COMPUTER(computer);

	/* A command that ran on a machine that was not up is the bug. */
	if (clawt_computer_get_state(computer) != CLAWT_COMPUTER_STATE_RUNNING) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_EXEC,
		                    "the machine is not running");
		return NULL;
	}

	g_atomic_int_inc(&self->execs);

	return clawt_exec_result_new(0, argv[0], "");
}

static ClawtComputerType
slow_type(
	ClawtComputer *computer
){
	(void)computer;

	return CLAWT_COMPUTER_CONTAINER;
}

static void
test_slow_computer_class_init(
	TestSlowComputerClass *klass
){
	ClawtComputerClass *computer_class;

	computer_class = CLAWT_COMPUTER_CLASS(klass);
	computer_class->start = slow_start;
	computer_class->stop = slow_stop;
	computer_class->exec = slow_exec;
	computer_class->get_computer_type = slow_type;
}

static void
test_slow_computer_init(
	TestSlowComputer *self
){
	self->start_ms = 300;
}

static TestSlowComputer *
slow_computer_new(
	gboolean first_use
){
	TestSlowComputer *self;

	self = (TestSlowComputer *)g_object_new(TEST_TYPE_SLOW_COMPUTER, NULL);
	clawt_computer_bind_agent(CLAWT_COMPUTER(self), "chief");
	clawt_computer_set_start_on_first_use(CLAWT_COMPUTER(self), first_use);

	return self;
}

static gpointer
exec_thread(
	gpointer data
){
	static const gchar *const argv[] = { "true", NULL };
	ClawtComputer *computer;
	GError *error = NULL;
	ClawtExecResult *result;

	computer = CLAWT_COMPUTER(data);
	result = clawt_computer_exec(computer, argv, NULL, 5, NULL, &error);

	if (result != NULL) {
		clawt_exec_result_free(result);
		return NULL;
	}

	return error;
}

/*
 * Five commands arrive at once at a machine that has not started: it
 * starts once, all five run, and none of them ran before it was up.
 */
static void
test_lazy_start_once_for_concurrent_first_uses(
	void
){
	g_autoptr(TestSlowComputer) computer = slow_computer_new(TRUE);
	GThread *threads[5];
	gsize i;

	g_assert_cmpint(clawt_computer_get_state(CLAWT_COMPUTER(computer)), ==,
	                CLAWT_COMPUTER_STATE_ABSENT);

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("first-use", exec_thread, computer);

	for (i = 0; i < G_N_ELEMENTS(threads); i++) {
		GError *error = g_thread_join(threads[i]);

		g_assert_no_error(error);
	}

	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 1);
	g_assert_cmpint(g_atomic_int_get(&computer->execs), ==, 5);

	/* Up now, so the next command starts nothing. */
	g_assert_null(exec_thread(computer));
	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 1);

	/*
	 * Stopped by hand, it is started again by its next use: that is what
	 * on_first_use means, and it is documented as such.
	 */
	g_assert_true(clawt_computer_stop(CLAWT_COMPUTER(computer), NULL));
	g_assert_null(exec_thread(computer));
	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 2);
}

/*
 * A start that fails fails every command that was waiting on it, with
 * the reason and with what kind of start it was -- and the next command
 * is a new wave that tries again, rather than inheriting the failure.
 */
static void
test_lazy_start_failure_is_shared_then_retried(
	void
){
	g_autoptr(TestSlowComputer) computer = slow_computer_new(TRUE);
	GThread *threads[3];
	gsize i;

	computer->fail = TRUE;

	for (i = 0; i < G_N_ELEMENTS(threads); i++)
		threads[i] = g_thread_new("first-use", exec_thread, computer);

	for (i = 0; i < G_N_ELEMENTS(threads); i++) {
		GError *error = g_thread_join(threads[i]);

		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION);
		g_assert_nonnull(strstr(error->message, "first use"));
		g_assert_nonnull(strstr(error->message, "no image"));
		g_error_free(error);
	}

	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 1);
	g_assert_cmpint(g_atomic_int_get(&computer->execs), ==, 0);

	computer->fail = FALSE;
	g_assert_null(exec_thread(computer));
	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 2);
}

/*
 * A machine that starts with its agent is never started by a command:
 * a stopped one stays stopped and the command says so, as before.
 */
static void
test_eager_computer_is_not_started_by_a_command(
	void
){
	g_autoptr(TestSlowComputer) computer = slow_computer_new(FALSE);
	GError *error;

	error = exec_thread(computer);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_EXEC);
	g_error_free(error);

	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 0);
	g_assert_true(clawt_computer_ensure_started(CLAWT_COMPUTER(computer),
	                                            NULL));
	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 0);
}

typedef struct {
	GMainContext *context;
	gint          pending;
	gint          ok;
	gint          ticks;
} AsyncCount;

static void
on_async_exec(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AsyncCount *count;
	g_autoptr(ClawtExecResult) exec = NULL;
	g_autoptr(GError) error = NULL;

	count = (AsyncCount *)user_data;
	exec = clawt_computer_exec_finish(CLAWT_COMPUTER(source), result,
	                                  &error);
	g_assert_no_error(error);

	if (exec != NULL)
		count->ok++;

	count->pending--;
}

static gboolean
on_tick(
	gpointer user_data
){
	((AsyncCount *)user_data)->ticks++;

	return G_SOURCE_CONTINUE;
}

static gboolean
on_watchdog(
	gpointer user_data
){
	(void)user_data;

	g_error("the lazy start never finished");

	return G_SOURCE_REMOVE;
}

/*
 * The async path, which is the one the daemon uses: the start goes
 * through the lifecycle verb on a worker, so a timer on the caller's
 * loop keeps firing while the machine boots -- the proof that the boot
 * is not being waited on where an IPC handler runs -- and three commands
 * still share one start.
 */
static void
test_lazy_start_async_shares_and_does_not_hold_the_loop(
	void
){
	static const gchar *const argv[] = { "true", NULL };
	g_autoptr(TestSlowComputer) computer = slow_computer_new(TRUE);
	g_autoptr(GMainContext) context = g_main_context_new();
	GSource *tick;
	GSource *watchdog;
	AsyncCount count;
	gint i;

	computer->start_ms = 600;
	memset(&count, 0, sizeof(count));
	count.context = context;
	count.pending = 3;

	g_main_context_push_thread_default(context);

	tick = g_timeout_source_new(50);
	g_source_set_callback(tick, on_tick, &count, NULL);
	g_source_attach(tick, context);

	watchdog = g_timeout_source_new_seconds(20);
	g_source_set_callback(watchdog, on_watchdog, NULL, NULL);
	g_source_attach(watchdog, context);

	for (i = 0; i < 3; i++)
		clawt_computer_exec_async(CLAWT_COMPUTER(computer), argv, NULL, 5,
		                          context, NULL, on_async_exec, &count);

	while (count.pending > 0)
		g_main_context_iteration(context, TRUE);

	g_source_destroy(tick);
	g_source_unref(tick);
	g_source_destroy(watchdog);
	g_source_unref(watchdog);
	g_main_context_pop_thread_default(context);

	g_assert_cmpint(count.ok, ==, 3);
	g_assert_cmpint(g_atomic_int_get(&computer->starts), ==, 1);
	g_assert_cmpint(count.ticks, >=, 4);
}

/*
 * The factory reads computer.start, and only for a type where deferring
 * means something: a host computer's start is a local check that should
 * fail when the agent starts.
 */
static void
test_factory_reads_the_start_mode(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
		"agents:\n"
		"  - id: chief\n"
		"    computer:\n"
		"      type: container\n"
		"      start: on_first_use\n"
		"  - id: second\n"
		"    computer:\n"
		"      type: container\n", &error);
	g_autoptr(ClawtComputer) lazy = NULL;
	g_autoptr(ClawtComputer) eager = NULL;

	g_assert_no_error(error);

	lazy = clawt_computer_factory_create(
		clawt_config_get_agent(config, "chief"), NULL, bridge, &error);
	g_assert_no_error(error);
	eager = clawt_computer_factory_create(
		clawt_config_get_agent(config, "second"), NULL, bridge, &error);
	g_assert_no_error(error);

	g_assert_true(clawt_computer_get_start_on_first_use(lazy));
	g_assert_false(clawt_computer_get_start_on_first_use(eager));

	g_assert_true(clawt_computer_type_starts_lazily(CLAWT_COMPUTER_VM));
	g_assert_true(clawt_computer_type_starts_lazily(CLAWT_COMPUTER_SSH));
	g_assert_false(clawt_computer_type_starts_lazily(CLAWT_COMPUTER_HOST));
	g_assert_false(clawt_computer_type_starts_lazily(CLAWT_COMPUTER_NONE));

	/* The enumeration the clients walk, default first. */
	g_assert_cmpuint(clawt_computer_start_count(), ==, 2);
	g_assert_cmpstr(clawt_computer_start_nth_nick(0), ==, "on_agent_start");
	g_assert_cmpstr(clawt_computer_start_nth_nick(1), ==, "on_first_use");
}

/* ── Waking a machine ────────────────────────────────────────────── */

typedef struct {
	gchar *dir;
	gchar *count_file;
} WakeFixture;

static void
wake_fixture_set_up(
	WakeFixture   *fixture,
	gconstpointer  data
){
	(void)data;

	fixture->dir = g_dir_make_tmp("clawt-wake-XXXXXX", NULL);
	g_assert_nonnull(fixture->dir);
	fixture->count_file = g_build_filename(fixture->dir, "runs", NULL);
}

static void
wake_fixture_tear_down(
	WakeFixture   *fixture,
	gconstpointer  data
){
	(void)data;

	clawt_test_remove_tree(fixture->dir);
	g_free(fixture->dir);
	g_free(fixture->count_file);
}

/* A wake command: /bin/sh running @script, never a shell line we built. */
static ClawtWaker *
sh_waker(
	const gchar *script,
	guint        timeout_seconds
){
	const gchar *argv[4];

	argv[0] = "/bin/sh";
	argv[1] = "-c";
	argv[2] = script;
	argv[3] = NULL;

	return clawt_waker_new(argv, timeout_seconds);
}

static guint
lines_in(
	const gchar *path
){
	g_autofree gchar *text = NULL;

	if (!g_file_get_contents(path, &text, NULL, NULL))
		return 0;

	return clawt_test_count_substrings(text, "\n");
}

typedef struct {
	ClawtWaker *waker;
	gulong      delay_ms;
	gint64      waited;
	gboolean    ok;
} WakeCall;

static gpointer
wake_thread(
	gpointer data
){
	WakeCall *call;

	call = (WakeCall *)data;
	g_usleep(call->delay_ms * 1000);
	call->ok = clawt_waker_wake(call->waker, &call->waited, NULL);

	return NULL;
}

/*
 * Four commands find the machine asleep at once: the wake runs once and
 * all four get its answer. One arriving after that wave is a new wave.
 */
static void
test_wake_once_per_wave(
	WakeFixture   *fixture,
	gconstpointer  data
){
	g_autofree gchar *script = NULL;
	g_autoptr(ClawtWaker) waker = NULL;
	WakeCall calls[4];
	GThread *threads[4];
	gsize i;

	(void)data;

	script = g_strdup_printf("echo run >> '%s'; sleep 0.5",
	                         fixture->count_file);
	waker = sh_waker(script, 10);

	for (i = 0; i < G_N_ELEMENTS(calls); i++) {
		calls[i].waker = waker;
		calls[i].delay_ms = (gulong)(i * 50);
		calls[i].waited = -1;
		calls[i].ok = FALSE;
		threads[i] = g_thread_new("wake", wake_thread, &calls[i]);
	}

	for (i = 0; i < G_N_ELEMENTS(calls); i++) {
		g_thread_join(threads[i]);
		g_assert_true(calls[i].ok);
	}

	g_assert_cmpuint(lines_in(fixture->count_file), ==, 1);
	g_assert_cmpuint(clawt_waker_get_runs(waker), ==, 1);

	g_assert_true(clawt_waker_wake(waker, NULL, NULL));
	g_assert_cmpuint(lines_in(fixture->count_file), ==, 2);
	g_assert_cmpuint(clawt_waker_get_runs(waker), ==, 2);
}

/*
 * Each caller is charged only the time it waited: one that joined half
 * way through a one-second wake waited about half a second, not one.
 */
static void
test_wake_charges_each_caller_its_own_wait(
	WakeFixture   *fixture,
	gconstpointer  data
){
	g_autoptr(ClawtWaker) waker = NULL;
	WakeCall first;
	WakeCall late;
	GThread *a;
	GThread *b;

	(void)fixture;
	(void)data;

	waker = sh_waker("sleep 1", 10);

	memset(&first, 0, sizeof(first));
	memset(&late, 0, sizeof(late));
	first.waker = waker;
	late.waker = waker;
	late.delay_ms = 500;

	a = g_thread_new("wake-first", wake_thread, &first);
	b = g_thread_new("wake-late", wake_thread, &late);
	g_thread_join(a);
	g_thread_join(b);

	g_assert_true(first.ok);
	g_assert_true(late.ok);
	g_assert_cmpuint(clawt_waker_get_runs(waker), ==, 1);

	g_assert_cmpint(first.waited, >=, 900 * 1000);
	g_assert_cmpint(late.waited, <, first.waited - 250 * 1000);
	g_assert_cmpint(late.waited, >, 0);
}

/*
 * A wake that does not finish is killed at its deadline -- and so is
 * everything it started, which is why it gets a process group: here a
 * background sleep that would otherwise hold the pipe, and the wait,
 * for half a minute.
 */
static void
test_wake_timeout_kills_the_group(
	WakeFixture   *fixture,
	gconstpointer  data
){
	g_autofree gchar *pid_file = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *pid_text = NULL;
	g_autoptr(ClawtWaker) waker = NULL;
	g_autoptr(GError) error = NULL;
	gint64 began;
	gint64 waited = 0;
	GPid straggler;
	gint tries;

	(void)data;

	pid_file = g_build_filename(fixture->dir, "straggler", NULL);
	script = g_strdup_printf("sleep 30 & echo $! > '%s'; sleep 30",
	                         pid_file);
	waker = sh_waker(script, 1);

	began = g_get_monotonic_time();
	g_assert_false(clawt_waker_wake(waker, &waited, &error));

	/* Ended at the deadline, not when the sleeps would have. */
	g_assert_cmpint(g_get_monotonic_time() - began, <, 10 * G_USEC_PER_SEC);
	g_assert_cmpint(waited, >=, 900 * 1000);

	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT);
	g_assert_nonnull(strstr(error->message, "the wake command"));
	g_assert_nonnull(strstr(error->message, "1 seconds"));
	g_assert_nonnull(strstr(error->message, "everything it started"));

	g_assert_true(g_file_get_contents(pid_file, &pid_text, NULL, NULL));
	straggler = (GPid)g_ascii_strtoll(pid_text, NULL, 10);
	g_assert_cmpint(straggler, >, 0);

	/* Reaped by whoever inherited it; give that a moment. */
	for (tries = 0; tries < 50 && !clawt_process_has_exited(straggler);
	     tries++)
		g_usleep(100 * 1000);

	g_assert_true(clawt_process_has_exited(straggler));
}

/*
 * A wake that fails says so, and how: the status and what it wrote, or
 * that it could not be started at all.
 */
static void
test_wake_failure_says_how(
	WakeFixture   *fixture,
	gconstpointer  data
){
	(void)fixture;
	(void)data;

	{
		g_autoptr(ClawtWaker) waker =
			sh_waker("echo 'no route to the switch' >&2; exit 3", 10);
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_waker_wake(waker, NULL, &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED);
		g_assert_nonnull(strstr(error->message, "the wake command"));
		g_assert_nonnull(strstr(error->message, "failed"));
		g_assert_nonnull(strstr(error->message, "status 3"));
		g_assert_nonnull(strstr(error->message, "no route to the switch"));
	}

	{
		static const gchar *const argv[] = {
			"/nonexistent/clawt-wakeonlan", "aa:bb:cc:dd:ee:ff", NULL
		};
		g_autoptr(ClawtWaker) waker = clawt_waker_new(argv, 0);
		g_autoptr(GError) error = NULL;

		/* 0 is the cap, not "no limit". */
		g_assert_cmpuint(clawt_waker_get_timeout(waker), ==,
		                 CLAWT_WAKE_MAX_SECONDS);
		g_assert_false(clawt_waker_wake(waker, NULL, &error));
		g_assert_nonnull(strstr(error->message, "could not be started"));
		g_assert_nonnull(strstr(error->message, "aa:bb:cc:dd:ee:ff"));
	}

	{
		static const gchar *const argv[] = { "/bin/true", NULL };
		g_autoptr(ClawtWaker) waker = clawt_waker_new(argv, 9999);

		g_assert_cmpuint(clawt_waker_get_timeout(waker), ==,
		                 CLAWT_WAKE_MAX_SECONDS);
	}
}

/*
 * Only "nothing answered" is something a wake can fix. A refused key or
 * login came from a machine that is awake, and waking it would spend two
 * minutes before the same refusal.
 */
static void
test_ssh_wake_wanted(
	void
){
	g_assert_true(clawt_ssh_wake_wanted(
		255, "ssh: connect to host box port 22: Connection timed out"));
	g_assert_true(clawt_ssh_wake_wanted(
		255, "ssh: connect to host box port 22: No route to host"));
	g_assert_true(clawt_ssh_wake_wanted(255, NULL));
	g_assert_false(clawt_ssh_wake_wanted(255,
		"Host key verification failed."));
	g_assert_false(clawt_ssh_wake_wanted(255,
		"me@box: Permission denied (publickey)."));
	g_assert_false(clawt_ssh_wake_wanted(0, NULL));
	g_assert_false(clawt_ssh_wake_wanted(1, "Connection timed out"));
}

/*
 * The wake command is an argv end to end: a list in the config, split
 * like a shell from a client's one line (and refused, not split, when it
 * leans on a shell), shown back as the line that splits into the same
 * argv, and handed to the ssh computer as that argv.
 */
static void
test_wake_command_is_an_argv(
	void
){
	g_autoptr(GError) error = NULL;

	{
		g_auto(GStrv) argv = clawt_command_line_split(
			"wakeonlan -i 192.168.1.255 'aa:bb:cc:dd:ee:ff'", &error);
		g_autofree gchar *line = NULL;
		g_auto(GStrv) again = NULL;

		g_assert_no_error(error);
		g_assert_cmpuint(g_strv_length(argv), ==, 4);
		g_assert_cmpstr(argv[3], ==, "aa:bb:cc:dd:ee:ff");

		line = clawt_command_line_join((const gchar * const *)argv);
		g_assert_cmpstr(line, ==,
		                "wakeonlan -i 192.168.1.255 aa:bb:cc:dd:ee:ff");

		again = clawt_command_line_split(line, NULL);
		assert_strv((const gchar * const *)again,
		            (const gchar * const *)argv);
	}

	{
		static const gchar *const spaced[] = { "resume", "my box", NULL };
		g_autofree gchar *line = clawt_command_line_join(spaced);
		g_auto(GStrv) back = clawt_command_line_split(line, NULL);

		assert_strv((const gchar * const *)back, spaced);
	}

	{
		g_auto(GStrv) refused = clawt_command_line_split(
			"wakeonlan aa:bb | tee /tmp/log", &error);

		g_assert_null(refused);
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
		g_assert_nonnull(strstr(error->message, "pipe"));
		g_clear_error(&error);
	}

	{
		g_auto(GStrv) blank = clawt_command_line_split("   ", &error);

		g_assert_no_error(error);
		g_assert_nonnull(blank);
		g_assert_null(blank[0]);
	}

	/* Set from one string, stored as a list split into words. */
	{
		g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
			"agents:\n"
			"  - id: chief\n"
			"    computer:\n"
			"      type: ssh\n"
			"      ssh:\n"
			"        host: box\n", &error);
		ClawtAgentConfig *agent;
		g_auto(GStrv) stored = NULL;
		g_autoptr(ClawtComputer) computer = NULL;
		ClawtWaker *waker;

		g_assert_no_error(error);
		agent = clawt_config_get_agent(config, "chief");

		g_assert_true(clawt_agent_config_set_from_string(
			agent, "computer.ssh.wake_command",
			"wakeonlan 'aa:bb:cc:dd:ee:ff'"));
		stored = clawt_agent_config_get_string_list(
			agent, "computer.ssh.wake_command");
		g_assert_cmpuint(g_strv_length(stored), ==, 2);
		g_assert_cmpstr(stored[1], ==, "aa:bb:cc:dd:ee:ff");

		/* A shell line is refused rather than stored with its pipe. */
		g_assert_false(clawt_agent_config_set_from_string(
			agent, "computer.ssh.wake_command", "a | b"));

		computer = clawt_computer_factory_create(agent, NULL, NULL, &error);
		g_assert_no_error(error);

		waker = clawt_ssh_computer_get_waker(CLAWT_SSH_COMPUTER(computer));
		g_assert_nonnull(waker);
		g_assert_cmpstr(clawt_waker_get_argv(waker)[0], ==, "wakeonlan");
		g_assert_cmpuint(clawt_waker_get_timeout(waker), ==,
		                 CLAWT_WAKE_MAX_SECONDS);

		/* Cleared, there is no waker at all. */
		g_assert_true(clawt_agent_config_set_from_string(
			agent, "computer.ssh.wake_command", ""));
		g_assert_true(clawt_computer_factory_refresh(agent, computer,
		                                             &error));
		g_assert_null(clawt_ssh_computer_get_waker(
			CLAWT_SSH_COMPUTER(computer)));
	}
}

/*
 * The time a command waited on a wake travels on its result, and is the
 * command's own share -- never part of its timeout.
 */
static void
test_exec_result_carries_wake_time(
	void
){
	g_autoptr(ClawtExecResult) result = clawt_exec_result_new(0, "", "");
	g_autoptr(ClawtExecResult) copy = NULL;

	g_assert_cmpint(clawt_exec_result_get_wake_usec(result), ==, 0);
	clawt_exec_result_set_wake_usec(result, 1500000);
	copy = clawt_exec_result_copy(result);
	g_assert_cmpint(clawt_exec_result_get_wake_usec(copy), ==, 1500000);

	clawt_exec_result_set_wake_usec(result, -5);
	g_assert_cmpint(clawt_exec_result_get_wake_usec(result), ==, 0);
}

/* A bounded command that exits is not mistaken for one that timed out. */
static void
test_run_argv_bounded_reports_exit(
	void
){
	static const gchar *const argv[] = {
		"/bin/sh", "-c", "echo out; echo err >&2; exit 4", NULL
	};
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	g_autoptr(GError) error = NULL;
	gint status = 0;

	g_assert_true(clawt_run_argv_bounded(argv, 5, &status, &out, &err,
	                                     &error));
	g_assert_no_error(error);
	g_assert_cmpint(status, ==, 4);
	g_assert_cmpstr(out, ==, "out\n");
	g_assert_cmpstr(err, ==, "err\n");
}

gint
main(
	gint    argc,
	gchar **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/computer-limits/size-parse", test_size_parse);
	g_test_add_func("/computer-limits/validate", test_limits_validate);
	g_test_add_func("/computer-limits/swap-first", test_limits_swap_first);
	g_test_add_func("/computer-limits/podman-create-args",
	                test_limits_podman_create_args);
	g_test_add_func("/computer-limits/distrobox-flags",
	                test_limits_distrobox_flags);
	g_test_add_func("/computer-limits/podman-prefix-and-inspect",
	                test_limits_podman_prefix_and_inspect);
	g_test_add_func("/computer-limits/update-on-change",
	                test_limits_update_on_change);
	g_test_add_func("/computer-limits/reach-both-backends",
	                test_limits_reach_both_backends);
	g_test_add_func("/computer-limits/podman-integration",
	                test_limits_podman_integration);

	g_test_add_func("/lazy-start/once-for-concurrent-first-uses",
	                test_lazy_start_once_for_concurrent_first_uses);
	g_test_add_func("/lazy-start/failure-shared-then-retried",
	                test_lazy_start_failure_is_shared_then_retried);
	g_test_add_func("/lazy-start/eager-not-started-by-command",
	                test_eager_computer_is_not_started_by_a_command);
	g_test_add_func("/lazy-start/async-shares-and-keeps-the-loop",
	                test_lazy_start_async_shares_and_does_not_hold_the_loop);
	g_test_add_func("/lazy-start/factory-reads-start-mode",
	                test_factory_reads_the_start_mode);

	g_test_add("/wake/once-per-wave", WakeFixture, NULL,
	           wake_fixture_set_up, test_wake_once_per_wave,
	           wake_fixture_tear_down);
	g_test_add("/wake/charges-each-caller-its-wait", WakeFixture, NULL,
	           wake_fixture_set_up, test_wake_charges_each_caller_its_own_wait,
	           wake_fixture_tear_down);
	g_test_add("/wake/timeout-kills-the-group", WakeFixture, NULL,
	           wake_fixture_set_up, test_wake_timeout_kills_the_group,
	           wake_fixture_tear_down);
	g_test_add("/wake/failure-says-how", WakeFixture, NULL,
	           wake_fixture_set_up, test_wake_failure_says_how,
	           wake_fixture_tear_down);
	g_test_add_func("/wake/ssh-wake-wanted", test_ssh_wake_wanted);
	g_test_add_func("/wake/command-is-an-argv", test_wake_command_is_an_argv);
	g_test_add_func("/wake/exec-result-carries-wake-time",
	                test_exec_result_carries_wake_time);
	g_test_add_func("/wake/run-argv-bounded-reports-exit",
	                test_run_argv_bounded_reports_exit);

	return g_test_run();
}
