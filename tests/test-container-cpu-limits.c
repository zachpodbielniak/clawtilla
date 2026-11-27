/*
 * test-container-cpu-limits.c - CPU limits, three levels, live update, usage
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * VENTURE #26. Hermetic: the podman the limits run is a shell script this
 * test writes, first on PATH, which records every call and answers the
 * inspect from a file -- so the live-update-or-restart decision is
 * exercised end to end without a container. The rest is data: the
 * resolution order of the three levels, the validation, the update argv
 * for each key, the stats parser and the OOM sentence.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

/* Where the fake podman lives; put first on PATH once, in main(). */
static gchar *fake_dir = NULL;

/* ── Parsing the CPU values ─────────────────────────────────────── */

static void
test_cpus_parse(
	void
){
	static const struct {
		const gchar *text;
		gint64       nano;
	} good[] = {
		{ "4",           4000000000 },
		{ "2.5",         2500000000 },
		{ "0.25",         250000000 },
		{ "1.000000001", 1000000001 },
		{ "16",         16000000000 }
	};
	static const gchar *const bad[] = {
		"0", "0.0", "-1", "+2", ".5", "2.", "1e3", "four", "2,5",
		"1.0000000001", "", " 2"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		g_autoptr(GError) error = NULL;
		gint64 nano = 0;

		g_assert_true(clawt_container_cpus_parse(good[i].text, &nano, &error));
		g_assert_no_error(error);
		g_assert_cmpint(nano, ==, good[i].nano);
	}

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_cpus_parse(bad[i], NULL, &error));
		g_assert_nonnull(error);
	}
}

static void
test_cpuset_normalise(
	void
){
	static const struct {
		const gchar *text;
		const gchar *canonical;
	} good[] = {
		{ "0-7",         "0-7" },
		{ "0,1,2,3",     "0-3" },
		{ "3,1,2,0",     "0-3" },
		{ "0,2,4-6",     "0,2,4-6" },
		{ "4-6, 0 , 2",  "0,2,4-6" },
		{ "5",           "5" },
		{ "0-3,2-5",     "0-5" }
	};
	static const gchar *const bad[] = {
		"", ",", "7-3", "a", "-1", "0-", "1;2"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(good); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *canonical =
			clawt_container_cpuset_normalise(good[i].text, 20, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(canonical, ==, good[i].canonical);
	}

	for (i = 0; i < G_N_ELEMENTS(bad); i++) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *canonical =
			clawt_container_cpuset_normalise(bad[i], 20, &error);

		g_assert_null(canonical);
		g_assert_nonnull(error);
	}

	/* Past the host: CPU 20 on a 20-CPU host (0-19) is refused by number. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *canonical =
			clawt_container_cpuset_normalise("16-23", 20, &error);

		g_assert_null(canonical);
		g_assert_nonnull(strstr(error->message, "CPU 23"));
		g_assert_nonnull(strstr(error->message, "20"));
	}
}

/*
 * The weight -> shares mapping: the ends and podman's default exactly,
 * and monotonic in between, which is all a relative weight needs.
 */
static void
test_cpu_weight_to_shares(
	void
){
	gint64 last = 0;
	gint64 weight;

	g_assert_cmpint(clawt_container_cpu_weight_to_shares(0), ==, 0);
	g_assert_cmpint(clawt_container_cpu_weight_to_shares(1), ==, 2);
	g_assert_cmpint(clawt_container_cpu_weight_to_shares(100), ==, 1024);
	g_assert_cmpint(clawt_container_cpu_weight_to_shares(10000), ==, 262144);

	for (weight = 1; weight <= 10000; weight += 7) {
		gint64 shares = clawt_container_cpu_weight_to_shares(weight);

		g_assert_cmpint(shares, >=, last);
		g_assert_cmpint(shares, >=, 2);
		g_assert_cmpint(shares, <=, 262144);
		last = shares;
	}
}

/* ── Validation ─────────────────────────────────────────────────── */

/* One limit set from key=value pairs, NULL-terminated. */
static ClawtContainerLimits *
limits_from(
	const gchar *first_key,
	...
){
	ClawtContainerLimits *limits = clawt_container_limits_new();
	const gchar *key;
	va_list args;

	va_start(args, first_key);

	for (key = first_key; key != NULL; key = va_arg(args, const gchar *)) {
		const gchar *value = va_arg(args, const gchar *);
		g_autofree gchar *full = g_strconcat("computer.container.", key,
		                                     NULL);

		g_assert_true(clawt_container_limits_set_key(limits, full, value,
		                                             NULL));
	}

	va_end(args);

	return limits;
}

/*
 * The ticket's refusals, on a 20-CPU host: cpus=0, cpus=64,
 * memory_swap < memory, a cpuset past the last CPU, a weight outside
 * cgroup v2's range. Each refusal names its key.
 */
static void
test_validate_for_host(
	void
){
	static const struct {
		const gchar *key;
		const gchar *value;
		const gchar *named;
	} refused[] = {
		{ "cpus",       "0",     "computer.container.cpus" },
		{ "cpus",       "64",    "computer.container.cpus is 64, more than the 20" },
		{ "cpus",       "20.5",  "computer.container.cpus" },
		{ "cpuset",     "0-31",  "computer.container.cpuset" },
		{ "cpuset",     "20",    "computer.container.cpuset" },
		{ "cpu_weight", "10001", "computer.container.cpu_weight" },
		{ "cpu_weight", "-5",    "computer.container.cpu_weight" }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(refused); i++) {
		g_autoptr(ClawtContainerLimits) limits =
			limits_from(refused[i].key, refused[i].value, NULL);
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_limits_validate_for_host(limits, 20,
		                                                        &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
		g_assert_nonnull(strstr(error->message, refused[i].named));
	}

	/* memory_swap < memory, named. */
	{
		g_autoptr(ClawtContainerLimits) limits =
			limits_from("memory", "8g", "memory_swap", "6g", NULL);
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_limits_validate_for_host(limits, 20,
		                                                        &error));
		g_assert_nonnull(strstr(error->message,
		                        "computer.container.memory_swap"));
	}

	/* The ticket's own values pass: forge at 4 and 8g, a member at 6. */
	{
		g_autoptr(ClawtContainerLimits) limits = limits_from(
			"cpus", "6", "memory", "8g", "memory_swap", "8g",
			"cpu_weight", "200", "cpuset", "0-19", "pids_limit", "4096",
			NULL);
		g_autoptr(GError) error = NULL;

		g_assert_true(clawt_container_limits_validate_for_host(limits, 20,
		                                                       &error));
		g_assert_no_error(error);
	}

	/*
	 * One level on its own: a team's swap total with no memory of its own
	 * is fine -- the memory may be each member's -- but still has to be a
	 * size; an agent's effective set with the same gap is refused.
	 */
	{
		g_autoptr(ClawtContainerLimits) swap_only =
			limits_from("memory_swap", "16g", NULL);
		g_autoptr(ClawtContainerLimits) misspelt =
			limits_from("memory_swap", "16gigs", NULL);
		g_autoptr(GError) error = NULL;

		g_assert_true(clawt_container_limits_validate_level(swap_only, 20,
		                                                    &error));
		g_assert_no_error(error);
		g_assert_false(clawt_container_limits_validate_for_host(swap_only,
		                                                        20, NULL));
		g_assert_false(clawt_container_limits_validate_level(misspelt, 20,
		                                                     &error));
		g_assert_nonnull(strstr(error->message,
		                        "computer.container.memory_swap"));
	}

	/* A whole-number key that is not one is refused where it is read. */
	{
		g_autoptr(ClawtContainerLimits) limits = clawt_container_limits_new();
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_container_limits_set_key(
			limits, "computer.container.cpu_weight", "lots", &error));
		g_assert_nonnull(strstr(error->message,
		                        "computer.container.cpu_weight"));
	}
}

/*
 * A bad value that reached the daemon costs that one limit: the rest
 * still apply, and each refusal names its key.
 */
static void
test_drop_invalid(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "64", "memory", "8g", "memory_swap", "6g", "cpuset", "0-3",
		"pids_limit", "500", NULL);
	g_autoptr(GPtrArray) refusals = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(ClawtContainerLimits) kept =
		clawt_container_limits_drop_invalid(limits, 20, refusals);

	g_assert_null(clawt_container_limits_get_cpus(kept));
	g_assert_null(clawt_container_limits_get_memory_swap(kept));
	g_assert_cmpstr(clawt_container_limits_get_memory(kept), ==, "8g");
	g_assert_cmpstr(clawt_container_limits_get_cpuset(kept), ==, "0-3");
	g_assert_cmpint(clawt_container_limits_get_pids_limit(kept), ==, 500);

	g_assert_cmpuint(refusals->len, ==, 2);
	g_assert_true(g_str_has_prefix(g_ptr_array_index(refusals, 0),
	                               "computer.container.cpus"));
	g_assert_true(g_str_has_prefix(g_ptr_array_index(refusals, 1),
	                               "computer.container.memory_swap"));
}

/* ── The podman argv ────────────────────────────────────────────── */

static void
assert_strv(
	const gchar * const *have,
	const gchar * const *want
){
	gsize i;

	for (i = 0; want[i] != NULL; i++) {
		g_assert_nonnull(have);
		g_assert_cmpstr(have[i], ==, want[i]);
	}

	g_assert_null(have[i]);
}

/* Each key at create, in the spelling podman takes. */
static void
test_create_args(
	void
){
	static const gchar *const want[] = {
		"--memory=8g", "--cpus=2.5", "--cpu-shares=1024",
		"--cpuset-cpus=0-3,8", NULL
	};
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "2.5", "cpu_weight", "100", "cpuset", "8,0,1,2,3",
		"memory", "8g", NULL);
	g_auto(GStrv) args = clawt_container_limits_create_args(limits);

	assert_strv((const gchar * const *)args, want);
}

/*
 * The update argv for each key: sent when podman's value differs, in
 * podman's unit, and not when it matches -- so an unchanged limit costs
 * one inspect and no update.
 */
static void
test_update_args_each_key(
	void
){
	/* Fresh container: nothing set, running. */
	static const gchar *const fresh = "2048 0 0 0 0 true\n";

	{
		static const gchar *const want[] = {
			"podman", "update", "--cpus=4", "clawt-forge", NULL
		};
		g_autoptr(ClawtContainerLimits) limits = limits_from("cpus", "4",
		                                                     NULL);
		g_auto(GStrv) options =
			clawt_container_limits_update_args(limits, fresh);
		g_auto(GStrv) argv = clawt_container_limits_update_argv(
			"podman", NULL, "clawt-forge", (const gchar * const *)options);

		assert_strv((const gchar * const *)argv, want);
	}

	{
		g_autoptr(ClawtContainerLimits) limits =
			limits_from("cpu_weight", "100", NULL);
		g_auto(GStrv) options = NULL;
		gint64 shares = clawt_container_cpu_weight_to_shares(200);
		g_autofree gchar *expect = g_strdup_printf("--cpu-shares=%"
		                                           G_GINT64_FORMAT, shares);

		/* 100 is 1024 shares; podman has 2048, so it is sent. */
		options = clawt_container_limits_update_args(
			limits, "2048 0 0 0 2048 true\n");
		g_assert_cmpstr(options[0], ==, "--cpu-shares=1024");
		g_assert_null(options[1]);
		g_strfreev(g_steal_pointer(&options));

		/* 200 is whatever the mapping says, and only that is sent. */
		clawt_container_limits_set_cpu_weight(limits, 200);
		options = clawt_container_limits_update_args(limits, fresh);
		g_assert_cmpstr(options[0], ==, expect);
		g_assert_null(options[1]);
	}

	{
		static const gchar *const want[] = { "--cpuset-cpus=0-7", NULL };
		g_autoptr(ClawtContainerLimits) limits =
			limits_from("cpuset", "0,1,2,3,4,5,6,7", NULL);
		g_auto(GStrv) options =
			clawt_container_limits_update_args(limits, fresh);

		assert_strv((const gchar * const *)options, want);
	}

	/* What it already has, in another spelling: nothing at all. */
	{
		g_autoptr(ClawtContainerLimits) limits = limits_from(
			"cpus", "2.5", "cpu_weight", "100", "cpuset", "0,1,2,3",
			"memory", "1g", NULL);
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "2048 1073741824 2147483648 2500000000 1024 true 0-3\n");
		g_auto(GStrv) argv = clawt_container_limits_update_argv(
			"podman", NULL, "clawt-forge", (const gchar * const *)options);

		g_assert_cmpuint(g_strv_length(options), ==, 0);
		g_assert_null(argv);
	}

	/* Only the CPU count moved: only it is sent. */
	{
		static const gchar *const want[] = { "--cpus=6", NULL };
		g_autoptr(ClawtContainerLimits) limits = limits_from(
			"cpus", "6", "memory", "8g", NULL);
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "2048 8589934592 17179869184 4000000000 0 true\n");

		assert_strv((const gchar * const *)options, want);
	}

	/*
	 * The inspect output from before the CPU keys -- three numbers --
	 * still reads: the memory half is compared, every CPU key is sent.
	 */
	{
		static const gchar *const want[] = { "--cpus=4", NULL };
		g_autoptr(ClawtContainerLimits) limits = limits_from(
			"cpus", "4", "memory", "1g", NULL);
		g_auto(GStrv) options = clawt_container_limits_update_args(
			limits, "2048 1073741824 0");

		assert_strv((const gchar * const *)options, want);
		g_assert_false(clawt_container_limits_inspected_running(
			"2048 1073741824 0"));
		g_assert_true(clawt_container_limits_inspected_running(fresh));
		g_assert_false(clawt_container_limits_inspected_running(
			"2048 0 0 0 0 false"));
	}
}

/* ── Live update or restart, against a fake podman ──────────────── */

/* The fake's state: what inspect prints, and what an update refuses. */
static void
fake_says(
	const gchar *inspect,
	const gchar *refuse
){
	g_autofree gchar *inspect_path = g_build_filename(fake_dir, "inspect",
	                                                  NULL);
	g_autofree gchar *refuse_path = g_build_filename(fake_dir, "refuse",
	                                                 NULL);
	g_autofree gchar *calls_path = g_build_filename(fake_dir, "calls", NULL);

	g_assert_true(g_file_set_contents(inspect_path, inspect, -1, NULL));

	if (refuse != NULL)
		g_assert_true(g_file_set_contents(refuse_path, refuse, -1, NULL));
	else
		(void)g_unlink(refuse_path);

	(void)g_unlink(calls_path);
}

/* Every `podman update` the fake was asked for, one per line. */
static gchar **
fake_updates(
	void
){
	g_autofree gchar *calls_path = g_build_filename(fake_dir, "calls", NULL);
	g_autofree gchar *calls = NULL;
	g_auto(GStrv) lines = NULL;
	GPtrArray *out = g_ptr_array_new();
	gsize i;

	if (g_file_get_contents(calls_path, &calls, NULL, NULL)) {
		lines = g_strsplit(calls, "\n", -1);

		for (i = 0; lines[i] != NULL; i++)
			if (g_str_has_prefix(lines[i], "update "))
				g_ptr_array_add(out, g_strdup(lines[i]));
	}

	g_ptr_array_add(out, NULL);

	return (gchar **)g_ptr_array_free(out, FALSE);
}

static void
fake_write(
	void
){
	g_autofree gchar *path = g_build_filename(fake_dir, "podman", NULL);
	g_autofree gchar *script = NULL;

	/*
	 * Absolute paths throughout: the limits run podman with an
	 * allowlisted environment.
	 */
	script = g_strdup_printf(
		"#!/bin/sh\n"
		"dir='%s'\n"
		"printf '%%s\\n' \"$*\" >> \"$dir/calls\"\n"
		"case \"$*\" in\n"
		"*'container inspect'*) exec /usr/bin/cat \"$dir/inspect\" ;;\n"
		"update*)\n"
		"  if [ -e \"$dir/refuse\" ]; then\n"
		"    refuse=$(/usr/bin/cat \"$dir/refuse\")\n"
		"    case \"$*\" in *\"$refuse\"*)\n"
		"      echo \"Error: crun: cannot change $refuse while running\" >&2\n"
		"      exit 125 ;;\n"
		"    esac\n"
		"  fi\n"
		"  exit 0 ;;\n"
		"esac\n"
		"exit 0\n", fake_dir);

	g_assert_true(g_file_set_contents(path, script, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

/*
 * Running, and podman takes it all: one update, in place, said so.
 */
static void
test_apply_live(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "1", "memory", "1g", "cpuset", "0", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *note = NULL;
	g_auto(GStrv) updates = NULL;
	ClawtContainerLimitsOutcome outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

	fake_says("2048 0 0 0 0 true\n", NULL);

	g_assert_true(clawt_container_limits_apply(limits, NULL, "clawt-forge",
	                                           &outcome, &note, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, CLAWT_CONTAINER_LIMITS_APPLIED_LIVE);
	g_assert_nonnull(strstr(note, "in place"));

	updates = fake_updates();
	g_assert_cmpuint(g_strv_length(updates), ==, 1);
	g_assert_cmpstr(updates[0], ==,
	                "update --memory=1g --cpus=1 --cpuset-cpus=0 clawt-forge");
}

/*
 * Running, and podman refuses the cpuset: everything else is changed in
 * place, the cpuset waits for the restart, and the note says which and
 * why -- with podman's own reason.
 */
static void
test_apply_restart_required(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "1", "memory", "1g", "cpuset", "0", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *note = NULL;
	g_auto(GStrv) updates = NULL;
	ClawtContainerLimitsOutcome outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

	fake_says("2048 0 0 0 0 true\n", "--cpuset-cpus");

	g_assert_true(clawt_container_limits_apply(limits, NULL, "clawt-forge",
	                                           &outcome, &note, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, CLAWT_CONTAINER_LIMITS_RESTART_REQUIRED);
	g_assert_nonnull(strstr(note, "restart required"));
	g_assert_nonnull(strstr(note, "--cpuset-cpus=0"));
	g_assert_nonnull(strstr(note, "cannot change"));
	g_assert_nonnull(strstr(note, "--cpus=1 --memory=1g changed in place"));

	/* The whole set, then each change alone: three more. */
	updates = fake_updates();
	g_assert_cmpuint(g_strv_length(updates), ==, 4);
	g_assert_cmpstr(updates[1], ==, "update --cpus=1 clawt-forge");
	g_assert_cmpstr(updates[2], ==, "update --cpuset-cpus=0 clawt-forge");
	g_assert_cmpstr(updates[3], ==, "update --memory=1g clawt-forge");
}

/*
 * Stopped, and podman refuses: there is no later to defer to, so the
 * apply -- and with it the start -- fails, naming the reason.
 */
static void
test_apply_stopped_refused_fails(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "1", "cpuset", "0", NULL);
	g_autoptr(GError) error = NULL;
	ClawtContainerLimitsOutcome outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

	fake_says("2048 0 0 0 0 false\n", "--cpuset-cpus");

	g_assert_false(clawt_container_limits_apply(limits, NULL, "clawt-forge",
	                                            &outcome, NULL, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_COMPUTER_PROVISION);
	g_assert_nonnull(strstr(error->message, "could not be applied"));
}

/* Stopped and accepted: applied before the start. Unchanged: no update. */
static void
test_apply_stopped_and_unchanged(
	void
){
	g_autoptr(ClawtContainerLimits) limits = limits_from(
		"cpus", "1", "memory", "1g", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *note = NULL;
	g_auto(GStrv) updates = NULL;
	ClawtContainerLimitsOutcome outcome = CLAWT_CONTAINER_LIMITS_UNCHANGED;

	fake_says("2048 0 0 0 0 false\n", NULL);
	g_assert_true(clawt_container_limits_apply(limits, NULL, "clawt-forge",
	                                           &outcome, &note, &error));
	g_assert_cmpint(outcome, ==, CLAWT_CONTAINER_LIMITS_APPLIED);
	g_assert_nonnull(strstr(note, "stopped machine"));
	g_clear_pointer(&note, g_free);

	fake_says("2048 1073741824 2147483648 1000000000 0 true\n", NULL);
	g_assert_true(clawt_container_limits_apply(limits, NULL, "clawt-forge",
	                                           &outcome, &note, &error));
	g_assert_cmpint(outcome, ==, CLAWT_CONTAINER_LIMITS_UNCHANGED);
	g_assert_null(note);

	updates = fake_updates();
	g_assert_cmpuint(g_strv_length(updates), ==, 0);
}

/*
 * Through the container computer: the outcome is kept for agent.list,
 * and logged with the path it took.
 */
static void
test_computer_keeps_outcome(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(ClawtContainerLimits) limits = limits_from("cpus", "1", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *note = NULL;
	gint64 at = 0;

	computer = clawt_container_computer_new("forge", bridge,
	                                        "registry.fedoraproject.org/fedora:45");
	clawt_container_computer_set_name(CLAWT_CONTAINER_COMPUTER(computer),
	                                  "clawt-forge");
	clawt_container_computer_set_limits(CLAWT_CONTAINER_COMPUTER(computer),
	                                    limits);

	fake_says("2048 0 0 0 0 true\n", NULL);

	g_test_expect_message("Clawtilla", G_LOG_LEVEL_MESSAGE,
	                      "*limits applied with podman update (live)*");
	g_assert_true(clawt_container_computer_apply_limits(
		CLAWT_CONTAINER_COMPUTER(computer), &error));
	g_test_assert_expected_messages();
	g_assert_no_error(error);

	g_assert_cmpint(clawt_container_computer_get_limits_outcome(
	                    CLAWT_CONTAINER_COMPUTER(computer), &note, &at),
	                ==, CLAWT_CONTAINER_LIMITS_APPLIED_LIVE);
	g_assert_nonnull(strstr(note, "--cpus=1"));
	g_assert_cmpint(at, >, 0);
}

/* ── Three levels ───────────────────────────────────────────────── */

static const gchar *const fleet_yaml =
	"defaults:\n"
	"  container:\n"
	"    cpus: \"2\"\n"
	"    memory: 2g\n"
	"    pids_limit: 500\n"
	"teams:\n"
	"  - id: forge\n"
	"    computer:\n"
	"      container:\n"
	"        cpus: \"4\"\n"
	"        memory: 8g\n"
	"agents:\n"
	"  - id: hammer\n"
	"    team: forge\n"
	"    computer:\n"
	"      type: container\n"
	"      container:\n"
	"        cpus: \"6\"\n"
	"  - id: anvil\n"
	"    team: forge\n"
	"    computer:\n"
	"      type: container\n"
	"  - id: loner\n"
	"    computer:\n"
	"      type: container\n";

static void
assert_limit(
	ClawtConfig        *config,
	const gchar        *agent_id,
	const gchar        *key,
	const gchar        *value,
	ClawtSettingSource  source,
	const gchar        *team
){
	ClawtSettingSource got = CLAWT_SETTING_FROM_NONE;
	const gchar *got_team = NULL;
	g_autofree gchar *full = g_strconcat("computer.container.", key, NULL);
	const gchar *got_value = clawt_agent_config_get_container_limit(
		clawt_config_get_agent(config, agent_id), full, &got, &got_team);

	g_assert_cmpstr(got_value, ==, value);
	g_assert_cmpint(got, ==, source);
	g_assert_cmpstr(got_team, ==, team);
}

/*
 * Most specific wins, key by key: forge at cpus 4 / memory 8g, hammer's
 * own cpus 6 over it and the team's memory still under it, the fleet's
 * pids_limit beneath both, and an agent on no team straight to the fleet.
 */
static void
test_resolution_order(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config =
		clawt_config_load_from_string(fleet_yaml, &error);

	g_assert_no_error(error);

	assert_limit(config, "hammer", "cpus", "6", CLAWT_SETTING_FROM_AGENT,
	             NULL);
	assert_limit(config, "hammer", "memory", "8g", CLAWT_SETTING_FROM_TEAM,
	             "forge");
	assert_limit(config, "hammer", "pids_limit", "500",
	             CLAWT_SETTING_FROM_DEFAULTS, NULL);
	assert_limit(config, "hammer", "cpuset", NULL, CLAWT_SETTING_FROM_NONE,
	             NULL);

	assert_limit(config, "anvil", "cpus", "4", CLAWT_SETTING_FROM_TEAM,
	             "forge");
	assert_limit(config, "loner", "cpus", "2", CLAWT_SETTING_FROM_DEFAULTS,
	             NULL);
	assert_limit(config, "loner", "memory", "2g",
	             CLAWT_SETTING_FROM_DEFAULTS, NULL);

	/* And the factory applies exactly that. */
	{
		g_autoptr(ClawtContainerLimits) limits =
			clawt_computer_factory_limits(
				clawt_config_get_agent(config, "hammer"));

		g_assert_cmpstr(clawt_container_limits_get_cpus(limits), ==, "6");
		g_assert_cmpstr(clawt_container_limits_get_memory(limits), ==, "8g");
		g_assert_cmpint(clawt_container_limits_get_pids_limit(limits), ==,
		                500);
	}

	/* A team written through the API lands nested, and is read back. */
	g_assert_true(clawt_config_set_team_string(config, "forge",
	                                           "computer.container.cpuset",
	                                           "0-3"));
	assert_limit(config, "anvil", "cpuset", "0-3", CLAWT_SETTING_FROM_TEAM,
	             "forge");
	g_assert_true(clawt_config_set_team_string(config, "forge",
	                                           "computer.container.cpuset",
	                                           NULL));
	assert_limit(config, "anvil", "cpuset", NULL, CLAWT_SETTING_FROM_NONE,
	             NULL);

	g_assert_cmpstr(clawt_setting_source_nick(CLAWT_SETTING_FROM_TEAM), ==,
	                "team");
	{
		g_autofree gchar *fleet =
			clawt_container_limit_fleet_key("computer.container.cpus");

		g_assert_cmpstr(fleet, ==, "defaults.container.cpus");
	}
}

/* config validate, which is where a bad value is meant to be stopped. */
static void
assert_validate_refuses(
	const gchar *yaml,
	const gchar *named
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtConfig) config =
		clawt_config_load_from_string(yaml, &error);

	g_assert_no_error(error);
	g_assert_false(clawt_config_validate(config, &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);

	if (strstr(error->message, named) == NULL)
		g_error("'%s' does not name '%s'", error->message, named);
}

static void
test_config_validate(
	void
){
	assert_validate_refuses(
		"agents:\n"
		"  - id: hammer\n"
		"    computer:\n"
		"      type: container\n"
		"      container:\n"
		"        cpus: \"0\"\n",
		"agent hammer: computer.container.cpus");

	/* More CPUs than any host this runs on has. */
	assert_validate_refuses(
		"defaults:\n"
		"  container:\n"
		"    cpus: \"100000\"\n",
		"defaults.container.cpus is 100000, more than the");

	assert_validate_refuses(
		"teams:\n"
		"  - id: forge\n"
		"    computer:\n"
		"      container:\n"
		"        memory: 4g\n"
		"        memory_swap: 1g\n",
		"team forge: computer.container.memory_swap");

	/* Across levels: a team's swap total under an agent's own memory. */
	assert_validate_refuses(
		"teams:\n"
		"  - id: forge\n"
		"    computer:\n"
		"      container:\n"
		"        memory_swap: 6g\n"
		"agents:\n"
		"  - id: hammer\n"
		"    team: forge\n"
		"    computer:\n"
		"      type: container\n"
		"      container:\n"
		"        memory: 8g\n",
		"memory_swap from team forge");

	assert_validate_refuses(
		"agents:\n"
		"  - id: hammer\n"
		"    computer:\n"
		"      type: container\n"
		"      container:\n"
		"        cpuset: \"0-9999\"\n",
		"computer.container.cpuset");

	/* And the fleet above, on a host with at least six CPUs, passes. */
	if (clawt_host_cpu_count() >= 6) {
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtConfig) config =
			clawt_config_load_from_string(fleet_yaml, &error);

		g_assert_true(clawt_config_validate(config, &error));
		g_assert_no_error(error);
	}
}

/*
 * A bad value that reaches the daemon anyway -- a hand edit nobody
 * validated -- is a warning naming the key and where it came from, and
 * is not applied; the computer is still built with the rest.
 */
static void
test_bad_value_is_warned_not_fatal(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = NULL;
	g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
		"teams:\n"
		"  - id: forge\n"
		"    computer:\n"
		"      container:\n"
		"        memory_swap: 1g\n"
		"agents:\n"
		"  - id: hammer\n"
		"    team: forge\n"
		"    computer:\n"
		"      type: container\n"
		"      container:\n"
		"        memory: 4g\n"
		"        cpus: \"1\"\n", &error);
	const ClawtContainerLimits *held;

	g_assert_no_error(error);

	g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
	                      "agent hammer: computer.container.memory_swap is 1g*"
	                      "(set by team forge); not applied");
	computer = clawt_computer_factory_create(
		clawt_config_get_agent(config, "hammer"), NULL, bridge, &error);
	g_test_assert_expected_messages();

	g_assert_no_error(error);
	g_assert_nonnull(computer);

	held = clawt_container_computer_get_limits(
		CLAWT_CONTAINER_COMPUTER(computer));
	g_assert_nonnull(held);
	g_assert_cmpstr(clawt_container_limits_get_memory(held), ==, "4g");
	g_assert_null(clawt_container_limits_get_memory_swap(held));
	g_assert_cmpstr(clawt_container_limits_get_cpus(held), ==, "1");
}

/* ── Usage, pressure and OOM ────────────────────────────────────── */

static void
test_stats_parse(
	void
){
	g_autoptr(GPtrArray) first = NULL;
	g_autoptr(GPtrArray) second = NULL;
	g_autoptr(GHashTable) previous = NULL;
	ClawtContainerUsage *usage;

	{
		g_auto(GStrv) argv = clawt_container_usage_stats_argv(
			"podman", "unix:///run/user/1000/podman/podman.sock");

		g_assert_cmpstr(argv[0], ==, "podman");
		g_assert_cmpstr(argv[1], ==, "--url");
		g_assert_cmpstr(argv[3], ==, "stats");
		g_assert_cmpstr(argv[4], ==, "--no-stream");
		g_assert_cmpstr(argv[7], ==, CLAWT_CONTAINER_USAGE_STATS_FORMAT);
	}

	first = clawt_container_usage_parse(
		"abc123|clawt-forge|12.5|10000000000|100000000000|1073741824|"
		"8589934592|42\n"
		"garbage line\n"
		"def456|clawt-other|0|1|2|3|4\n", NULL, 1000);

	g_assert_cmpuint(first->len, ==, 1);
	usage = g_ptr_array_index(first, 0);
	g_assert_cmpstr(usage->name, ==, "clawt-forge");
	g_assert_cmpfloat(usage->cpu_percent, ==, 12.5);
	g_assert_cmpint(usage->memory_used, ==, 1073741824);
	g_assert_cmpint(usage->memory_limit, ==, 8589934592);
	g_assert_cmpint(usage->pids, ==, 42);
	g_assert_cmpint(usage->oom_kills, ==, -1);

	/*
	 * Ten seconds later, twenty CPU-seconds more: two CPUs flat out,
	 * 200%, from the deltas rather than podman's lifetime average.
	 */
	previous = g_hash_table_new(g_str_hash, g_str_equal);
	g_hash_table_insert(previous, usage->name, usage);
	second = clawt_container_usage_parse(
		"abc123|clawt-forge|3.0|30000000000|110000000000|1073741824|"
		"8589934592|40\n", previous, 2000);
	usage = g_ptr_array_index(second, 0);
	g_assert_cmpfloat_with_epsilon(usage->cpu_percent, 200.0, 0.001);
}

static void
test_cgroups_and_oom_counter(
	void
){
	g_autoptr(GHashTable) paths = clawt_container_usage_parse_cgroups(
		"clawt-forge|/user.slice/user-1000.slice/libpod-abc.scope\n"
		"/clawt-other|/machine.slice/libpod-def.scope\n"
		"broken\n");
	g_autofree gchar *root = g_dir_make_tmp("clawt-cgroup-XXXXXX", NULL);
	g_autofree gchar *scope = NULL;
	g_autofree gchar *events = NULL;

	g_assert_cmpstr(g_hash_table_lookup(paths, "clawt-forge"), ==,
	                "/user.slice/user-1000.slice/libpod-abc.scope");
	g_assert_cmpstr(g_hash_table_lookup(paths, "clawt-other"), ==,
	                "/machine.slice/libpod-def.scope");
	g_assert_cmpuint(g_hash_table_size(paths), ==, 2);

	scope = g_build_filename(root, "libpod-abc.scope", NULL);
	g_assert_cmpint(g_mkdir_with_parents(scope, 0700), ==, 0);
	events = g_build_filename(scope, "memory.events", NULL);
	g_assert_true(g_file_set_contents(events,
		"low 0\nhigh 0\nmax 12\noom 3\noom_kill 2\noom_group_kill 0\n",
		-1, NULL));

	g_assert_cmpint(clawt_container_usage_read_oom_kills(
	                    root, "/libpod-abc.scope"), ==, 2);
	g_assert_cmpint(clawt_container_usage_read_oom_kills(
	                    root, "/nowhere.scope"), ==, -1);
	g_assert_cmpint(clawt_container_usage_read_oom_kills(
	                    root, "/../etc"), ==, -1);
	g_assert_cmpint(clawt_container_usage_read_oom_kills(root, NULL), ==, -1);

	clawt_test_remove_tree(root);
}

/*
 * What the computer makes of its samples: the first OOM count is a
 * baseline, a rise is news (and an incident upstream), a new container
 * starts again; two samples at the limit is pressure, one is a spike.
 */
static void
test_take_usage(
	void
){
	g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
	g_autoptr(ClawtComputer) computer = clawt_container_computer_new(
		"forge", bridge, "registry.fedoraproject.org/fedora:45");
	ClawtContainerComputer *container = CLAWT_CONTAINER_COMPUTER(computer);
	g_autoptr(ClawtContainerLimits) limits = limits_from("memory", "1g",
	                                                     NULL);
	ClawtContainerUsage usage;
	gboolean changed = FALSE;

	clawt_container_computer_set_limits(container, limits);

	memset(&usage, 0, sizeof(usage));
	usage.container_id = (gchar *)"abc";
	usage.name = (gchar *)"clawt-forge";
	usage.memory_used = 100 * 1024 * 1024;
	usage.oom_kills = 5;

	g_assert_cmpint(clawt_container_computer_take_usage(container, &usage,
	                                                    &changed), ==, 0);
	g_assert_false(changed);

	usage.oom_kills = 7;
	g_assert_cmpint(clawt_container_computer_take_usage(container, &usage,
	                                                    &changed), ==, 2);

	/* Recreated: another id, so 1 is a new baseline, not a fall. */
	usage.container_id = (gchar *)"def";
	usage.oom_kills = 1;
	g_assert_cmpint(clawt_container_computer_take_usage(container, &usage,
	                                                    NULL), ==, 0);

	/* At the limit: once is a spike, twice is pressure. */
	usage.memory_used = 1000 * 1024 * 1024;
	(void)clawt_container_computer_take_usage(container, &usage, &changed);
	g_assert_false(changed);
	g_assert_false(clawt_container_computer_get_memory_pressure(container));
	(void)clawt_container_computer_take_usage(container, &usage, &changed);
	g_assert_true(changed);
	g_assert_true(clawt_container_computer_get_memory_pressure(container));

	/* Stopped: out of the sample, and no longer under pressure. */
	(void)clawt_container_computer_take_usage(container, NULL, &changed);
	g_assert_true(changed);
	g_assert_false(clawt_container_computer_get_memory_pressure(container));
	g_assert_null(clawt_container_computer_get_usage(container));

	/* One OOM report per kill however it was heard about. */
	g_assert_true(clawt_container_computer_claim_oom_report(container,
	                                                        1000000));
	g_assert_false(clawt_container_computer_claim_oom_report(container,
	                                                         2000000));
	g_assert_true(clawt_container_computer_claim_oom_report(
		container, 1000000 + 91 * G_USEC_PER_SEC));
}

/* The incident's reason names the limit, and its kind says what it is. */
static void
test_oom_reason(
	void
){
	g_autofree gchar *with = clawt_container_usage_oom_reason(
		"8g", "clawt-forge", 1);
	g_autofree gchar *without = clawt_container_usage_oom_reason(
		NULL, "clawt-forge", 0);
	g_autofree gchar *size = clawt_container_usage_format_bytes(
		(gint64)7 * 1024 * 1024 * 1024 + 800 * 1024 * 1024);

	g_assert_true(g_str_has_prefix(with,
	                               "killed: out of memory at 8g (container "
	                               "clawt-forge, 1 process killed)"));
	g_assert_nonnull(strstr(with, "computer.container.memory"));
	g_assert_true(g_str_has_prefix(without, "killed: out of memory "
	                                        "(container clawt-forge)"));
	g_assert_cmpstr(size, ==, "7.8g");

	g_assert_cmpstr(clawt_incident_kind_label(CLAWT_INCIDENT_OUT_OF_MEMORY),
	                ==, "ran out of memory");
	g_assert_cmpstr(clawt_enum_to_nick(CLAWT_TYPE_INCIDENT_KIND,
	                                   CLAWT_INCIDENT_OUT_OF_MEMORY),
	                ==, "out-of-memory");
}

gint
main(
	gint    argc,
	gchar **argv
){
	g_autofree gchar *path = NULL;
	gint status;

	g_test_init(&argc, &argv, NULL);

	/* Ours first, before any thread exists: setenv() is not thread-safe. */
	fake_dir = g_dir_make_tmp("clawt-cpu-limits-XXXXXX", NULL);
	g_assert_nonnull(fake_dir);
	fake_write();
	path = g_strdup_printf("%s:%s", fake_dir,
	                       g_getenv("PATH") != NULL ? g_getenv("PATH")
	                                                : "/usr/bin:/bin");
	g_setenv("PATH", path, TRUE);

	g_test_add_func("/cpu-limits/parse/cpus", test_cpus_parse);
	g_test_add_func("/cpu-limits/parse/cpuset", test_cpuset_normalise);
	g_test_add_func("/cpu-limits/parse/weight-to-shares",
	                test_cpu_weight_to_shares);
	g_test_add_func("/cpu-limits/validate/host", test_validate_for_host);
	g_test_add_func("/cpu-limits/validate/drop-invalid", test_drop_invalid);
	g_test_add_func("/cpu-limits/argv/create", test_create_args);
	g_test_add_func("/cpu-limits/argv/update-each-key",
	                test_update_args_each_key);
	g_test_add_func("/cpu-limits/apply/live", test_apply_live);
	g_test_add_func("/cpu-limits/apply/restart-required",
	                test_apply_restart_required);
	g_test_add_func("/cpu-limits/apply/stopped-refused-fails",
	                test_apply_stopped_refused_fails);
	g_test_add_func("/cpu-limits/apply/stopped-and-unchanged",
	                test_apply_stopped_and_unchanged);
	g_test_add_func("/cpu-limits/apply/computer-keeps-outcome",
	                test_computer_keeps_outcome);
	g_test_add_func("/cpu-limits/levels/resolution-order",
	                test_resolution_order);
	g_test_add_func("/cpu-limits/levels/config-validate",
	                test_config_validate);
	g_test_add_func("/cpu-limits/levels/bad-value-warned",
	                test_bad_value_is_warned_not_fatal);
	g_test_add_func("/cpu-limits/usage/stats-parse", test_stats_parse);
	g_test_add_func("/cpu-limits/usage/oom-counter",
	                test_cgroups_and_oom_counter);
	g_test_add_func("/cpu-limits/usage/take", test_take_usage);
	g_test_add_func("/cpu-limits/usage/oom-reason", test_oom_reason);

	status = g_test_run();

	clawt_test_remove_tree(fake_dir);
	g_free(fake_dir);

	return status;
}
