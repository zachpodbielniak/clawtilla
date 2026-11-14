/*
 * test-trigger-match.c - The payload filter, the post mode and the cap
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: no daemon, no socket.  The rules are library functions, so
 * they are tested here against parsed JSON and loaded configs; what the
 * daemon does with them is in test-daemon.c.
 */

#include <clawtilla.h>
#include <string.h>

#include "clawt-test-util.h"

/* A body shaped like a real pull-request delivery. */
static const gchar PAYLOAD[] =
	"{\"action\": \"opened\","
	" \"number\": 42,"
	" \"draft\": false,"
	" \"pull_request\": {\"title\": \"Release 1.2: the STRASSE fix\","
	"                    \"user\": {\"login\": \"Alice\"}},"
	" \"labels\": [{\"name\": \"bug\"}, {\"name\": \"Deploy-Now\"}],"
	" \"commits\": [{\"message\": \"first\"}, {\"message\": \"second\"}],"
	" \"note\": null}";

static JsonNode *
parse(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	json_parser_load_from_data(parser, text, -1, &error);
	g_assert_no_error(error);

	return json_node_copy(json_parser_get_root(parser));
}

static ClawtConfig *
load(const gchar *yaml)
{
	g_autoptr(GError) error = NULL;
	ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

	g_assert_no_error(error);
	g_assert_nonnull(config);

	return config;
}

/* Whether any load warning contains @needle. */
static gboolean
warned(ClawtConfig *config, const gchar *needle)
{
	GPtrArray *warnings = clawt_config_get_warnings(config);
	guint i;

	for (i = 0; warnings != NULL && i < warnings->len; i++) {
		if (strstr(g_ptr_array_index(warnings, i), needle) != NULL)
			return TRUE;
	}

	return FALSE;
}

static GPtrArray *
pairs(const gchar *first_path, ...)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_trigger_match_free);
	const gchar *path = first_path;
	va_list args;

	va_start(args, first_path);

	while (path != NULL) {
		const gchar *value = va_arg(args, const gchar *);

		g_ptr_array_add(out, clawt_trigger_match_new(path, value));
		path = va_arg(args, const gchar *);
	}

	va_end(args);

	return out;
}

/* ── Path lookup ─────────────────────────────────────────────────── */

static void
test_lookup_nested_arrays_and_missing(void)
{
	g_autoptr(JsonNode) root = parse(PAYLOAD);
	g_autofree gchar *title = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *names = NULL;
	g_autofree gchar *number = NULL;
	g_autofree gchar *draft = NULL;
	g_autofree gchar *subtree = NULL;
	g_autofree gchar *null_value = NULL;
	g_autofree gchar *missing = NULL;
	g_autofree gchar *past_end = NULL;
	g_autofree gchar *into_scalar = NULL;

	/* Nested objects. */
	title = clawt_trigger_match_lookup(root, "pull_request.title");
	g_assert_cmpstr(title, ==, "Release 1.2: the STRASSE fix");
	login = clawt_trigger_match_lookup(root, "pull_request.user.login");
	g_assert_cmpstr(login, ==, "Alice");

	/* A number indexes an array; a name on an array looks in each. */
	second = clawt_trigger_match_lookup(root, "commits.1.message");
	g_assert_cmpstr(second, ==, "second");
	names = clawt_trigger_match_lookup(root, "labels.name");
	g_assert_cmpstr(names, ==, "bug\nDeploy-Now");

	/* Scalars in their JSON spelling, structure as compact JSON. */
	number = clawt_trigger_match_lookup(root, "number");
	g_assert_cmpstr(number, ==, "42");
	draft = clawt_trigger_match_lookup(root, "draft");
	g_assert_cmpstr(draft, ==, "false");
	null_value = clawt_trigger_match_lookup(root, "note");
	g_assert_cmpstr(null_value, ==, "null");
	subtree = clawt_trigger_match_lookup(root, "pull_request.user");
	g_assert_nonnull(strstr(subtree, "\"login\""));

	/* Absent, an index past the end, and a name inside a scalar. */
	missing = clawt_trigger_match_lookup(root, "pull_request.body");
	g_assert_null(missing);
	past_end = clawt_trigger_match_lookup(root, "commits.9.message");
	g_assert_null(past_end);
	into_scalar = clawt_trigger_match_lookup(root, "action.length");
	g_assert_null(into_scalar);
}

/* ── Containment ─────────────────────────────────────────────────── */

static void
test_contains_ignores_case(void)
{
	g_assert_true(clawt_trigger_match_contains("Release 1.2", "release"));
	g_assert_true(clawt_trigger_match_contains("release", "RELEASE"));
	g_assert_true(clawt_trigger_match_contains("Deploy-Now", "deploy-now"));

	/* Folding, not ASCII lowering: a capital Ü finds a small one. */
	g_assert_true(clawt_trigger_match_contains("\303\234BER", "\303\274ber"));

	g_assert_false(clawt_trigger_match_contains("Release", "hotfix"));
	g_assert_false(clawt_trigger_match_contains(NULL, "x"));
	g_assert_false(clawt_trigger_match_contains("x", NULL));
}

/* ── All pairs must match ────────────────────────────────────────── */

static void
test_all_pairs_must_match(void)
{
	g_autoptr(GPtrArray) both = pairs("action", "OPEN",
	                                  "labels.name", "deploy", NULL);
	g_autoptr(GPtrArray) second_misses = pairs("action", "opened",
	                                           "pull_request.title", "hotfix",
	                                           NULL);
	g_autoptr(GPtrArray) absent = pairs("action", "opened",
	                                    "pull_request.body", "x", NULL);
	g_autoptr(GPtrArray) none = g_ptr_array_new();
	g_autofree gchar *reason = NULL;
	g_autofree gchar *absent_reason = NULL;
	g_autofree gchar *not_json = NULL;

	g_assert_true(clawt_trigger_match_evaluate(both, PAYLOAD, NULL));

	/* The second pair fails, and the reason names it and what was there. */
	g_assert_false(clawt_trigger_match_evaluate(second_misses, PAYLOAD,
	                                            &reason));
	g_assert_nonnull(strstr(reason, "pair 2 of 2"));
	g_assert_nonnull(strstr(reason, "pull_request.title"));
	g_assert_nonnull(strstr(reason, "hotfix"));
	g_assert_nonnull(strstr(reason, "Release 1.2"));

	g_assert_false(clawt_trigger_match_evaluate(absent, PAYLOAD,
	                                            &absent_reason));
	g_assert_nonnull(strstr(absent_reason, "nothing at 'pull_request.body'"));

	/* No filter matches anything, even a body that is not JSON. */
	g_assert_true(clawt_trigger_match_evaluate(none, "not json", NULL));

	/* A filter against a body it cannot read matches nothing. */
	g_assert_false(clawt_trigger_match_evaluate(both, "not json",
	                                            &not_json));
	g_assert_nonnull(strstr(not_json, "not JSON"));
}

/* ── What a pair may be ──────────────────────────────────────────── */

static void
test_bad_paths_and_values_are_refused(void)
{
	static const gchar *const bad_paths[] = {
		"", ".action", "action.", "pull_request..title", "has space",
		"a=b", "tab\there", NULL
	};
	g_autofree gchar *deep = NULL;
	GString *built;
	guint i;

	for (i = 0; bad_paths[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		g_assert_false(clawt_trigger_match_check(bad_paths[i], "x", &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	}

	/* Thirty-three parts is past the depth limit. */
	built = g_string_new("a");

	for (i = 0; i < 32; i++)
		g_string_append(built, ".a");

	deep = g_string_free(built, FALSE);
	g_assert_false(clawt_trigger_match_check(deep, "x", NULL));

	/* An empty value would match everything, so it is no filter at all. */
	g_assert_false(clawt_trigger_match_check("action", "", NULL));
	g_assert_false(clawt_trigger_match_check("action", NULL, NULL));

	g_assert_true(clawt_trigger_match_check("labels.0.name", "bug", NULL));
	g_assert_true(clawt_trigger_match_check("x-github-hook", "a=b", NULL));
}

/* ── From a client ───────────────────────────────────────────────── */

static void
test_from_json_every_spelling_and_the_limit(void)
{
	g_autoptr(JsonNode) objects = parse(
		"[{\"path\": \"action\", \"value\": \"opened\"},"
		" {\"path\": \"number\", \"value\": 42},"
		" {\"path\": \"\", \"value\": \"\"}]");
	g_autoptr(JsonNode) strings = parse("[\"action=opened\", \"a.b=x=y\"]");
	g_autoptr(JsonNode) lines = parse("\"action=opened\\n\\nnumber=4\"");
	g_autoptr(JsonNode) null_node = parse("null");
	g_autoptr(JsonNode) empty = parse("[]");
	g_autoptr(JsonNode) bad = parse("[\"no-equals-sign\"]");
	g_autoptr(GPtrArray) from_objects = NULL;
	g_autoptr(GPtrArray) from_strings = NULL;
	g_autoptr(GPtrArray) from_lines = NULL;
	g_autoptr(GPtrArray) from_null = NULL;
	g_autoptr(GPtrArray) from_empty = NULL;
	g_autoptr(GPtrArray) from_bad = NULL;
	g_autoptr(GPtrArray) too_many = NULL;
	g_autoptr(JsonNode) eleven = NULL;
	g_autoptr(GError) error = NULL;
	GString *json;
	guint i;

	/* Objects; a number is its spelling; a wholly blank row is no pair. */
	from_objects = clawt_trigger_match_from_json(objects, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_objects->len, ==, 2);
	g_assert_cmpstr(((ClawtTriggerMatch *)from_objects->pdata[1])->value, ==,
	                "42");

	/* Strings split at the first '=', since a path cannot hold one. */
	from_strings = clawt_trigger_match_from_json(strings, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_strings->len, ==, 2);
	g_assert_cmpstr(((ClawtTriggerMatch *)from_strings->pdata[1])->path, ==,
	                "a.b");
	g_assert_cmpstr(((ClawtTriggerMatch *)from_strings->pdata[1])->value, ==,
	                "x=y");

	from_lines = clawt_trigger_match_from_json(lines, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_lines->len, ==, 2);

	/* null and [] both clear. */
	from_null = clawt_trigger_match_from_json(null_node, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_null->len, ==, 0);
	from_empty = clawt_trigger_match_from_json(empty, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_empty->len, ==, 0);

	from_bad = clawt_trigger_match_from_json(bad, &error);
	g_assert_null(from_bad);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* Ten is the limit; the eleventh is refused, not truncated. */
	json = g_string_new("[");

	for (i = 0; i < CLAWT_TRIGGER_MATCH_MAX + 1; i++)
		g_string_append_printf(json, "%s\"f%u=x\"", i > 0 ? "," : "", i);

	g_string_append(json, "]");
	eleven = parse(json->str);
	g_string_free(json, TRUE);

	too_many = clawt_trigger_match_from_json(eleven, &error);
	g_assert_null(too_many);
	g_assert_nonnull(strstr(error->message, "at most 10"));
}

/* ── In the config ───────────────────────────────────────────────── */

static void
test_config_match_reads_clears_and_limits(void)
{
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GPtrArray) read = NULL;
	g_autoptr(GPtrArray) cleared = NULL;
	g_autoptr(GPtrArray) emptied = NULL;
	g_autoptr(GPtrArray) written = NULL;
	g_autoptr(GPtrArray) reread = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *eleven = NULL;
	GString *yaml;
	ClawtTrigger *trigger;
	guint i;

	config = load(
		"triggers:\n"
		"  - id: ok\n    agent: a\n    instructions: x\n"
		"    match:\n"
		"      - path: pull_request.title\n        value: release\n"
		"      - path: action\n        value: opened\n"
		"  - id: nulled\n    agent: a\n    instructions: x\n"
		"    match: null\n"
		"  - id: emptied\n    agent: a\n    instructions: x\n"
		"    match: []\n"
		"  - id: bad\n    agent: a\n    instructions: x\n"
		"    match:\n      - path: 'a..b'\n        value: x\n");

	read = clawt_trigger_get_match(clawt_config_get_trigger(config, "ok"),
	                               &error);
	g_assert_no_error(error);
	g_assert_cmpuint(read->len, ==, 2);
	g_assert_cmpstr(((ClawtTriggerMatch *)read->pdata[0])->path, ==,
	                "pull_request.title");

	/* An explicit null and an empty list are both no filter. */
	cleared = clawt_trigger_get_match(
		clawt_config_get_trigger(config, "nulled"), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(cleared->len, ==, 0);
	emptied = clawt_trigger_get_match(
		clawt_config_get_trigger(config, "emptied"), &error);
	g_assert_no_error(error);
	g_assert_cmpuint(emptied->len, ==, 0);

	/* A bad path is an error, a warning at load, and fails validation. */
	g_assert_null(clawt_trigger_get_match(
		clawt_config_get_trigger(config, "bad"), &error));
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
	g_clear_error(&error);
	g_assert_true(warned(config, "trigger 'bad'"));
	g_assert_false(clawt_config_validate(config, &error));
	g_assert_nonnull(strstr(error->message, "trigger 'bad'"));
	g_clear_error(&error);

	/* A bad filter matches nothing rather than everything. */
	{
		g_autoptr(ClawtTriggerEvent) event = clawt_trigger_event_new(
			CLAWT_TRIGGER_PROVIDER_GENERIC, "push", "d1");
		g_autofree gchar *reason = NULL;

		clawt_trigger_event_set_payload(event, "{\"a\": {\"b\": \"x\"}}");
		g_assert_false(clawt_trigger_accepts_delivery(
			clawt_config_get_trigger(config, "bad"), event, &reason));
		g_assert_nonnull(strstr(reason, "cannot be used"));
	}

	/* Written as mappings and read back; cleared by removal. */
	trigger = clawt_config_get_trigger(config, "nulled");
	written = pairs("repository.name", "clawtilla", NULL);
	g_assert_true(clawt_trigger_set_match(trigger, written));
	reread = clawt_trigger_get_match(trigger, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(reread->len, ==, 1);
	g_assert_cmpstr(((ClawtTriggerMatch *)reread->pdata[0])->value, ==,
	                "clawtilla");
	g_assert_true(clawt_trigger_set_match(trigger, NULL));
	g_assert_false(clawt_trigger_has_key(trigger, "match"));

	/* Eleven pairs written by hand are refused at load, not trimmed. */
	g_clear_object(&config);
	yaml = g_string_new("triggers:\n  - id: many\n    agent: a\n"
	                    "    instructions: x\n    match:\n");

	for (i = 0; i < CLAWT_TRIGGER_MATCH_MAX + 1; i++)
		g_string_append_printf(yaml, "      - path: f%u\n        value: x\n",
		                       i);

	eleven = g_string_free(yaml, FALSE);
	config = load(eleven);
	g_assert_null(clawt_trigger_get_match(
		clawt_config_get_trigger(config, "many"), &error));
	g_assert_nonnull(strstr(error->message, "at most 10"));
	g_clear_error(&error);
	g_assert_true(warned(config, "at most 10"));
}

/* ── Mode ────────────────────────────────────────────────────────── */

static void
test_post_needs_a_room_and_unknown_mode_warns(void)
{
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	config = load(
		"triggers:\n"
		"  - id: roomless\n    agent: a\n    instructions: x\n"
		"    mode: post\n"
		"  - id: posting\n    agent: a\n    instructions: x\n"
		"    mode: post\n    room: ops\n"
		"  - id: typo\n    agent: a\n    instructions: x\n"
		"    mode: pots\n"
		"  - id: plain\n    agent: a\n    instructions: x\n");

	g_assert_cmpint(clawt_trigger_get_mode(
		clawt_config_get_trigger(config, "posting")), ==,
		CLAWT_TRIGGER_MODE_POST);
	g_assert_cmpint(clawt_trigger_get_mode(
		clawt_config_get_trigger(config, "plain")), ==,
		CLAWT_TRIGGER_MODE_RUN);

	/* A post with nowhere to go is refused, and says what to do. */
	g_assert_false(clawt_trigger_validate(
		clawt_config_get_trigger(config, "roomless"), &error));
	g_assert_nonnull(strstr(error->message, "room"));
	g_clear_error(&error);
	g_assert_true(warned(config, "trigger 'roomless'"));
	g_assert_false(clawt_config_validate(config, NULL));

	g_assert_true(clawt_trigger_validate(
		clawt_config_get_trigger(config, "posting"), NULL));

	/* A misspelt mode reads as run, and the load says so. */
	g_assert_cmpint(clawt_trigger_get_mode(
		clawt_config_get_trigger(config, "typo")), ==,
		CLAWT_TRIGGER_MODE_RUN);
	g_assert_true(warned(config, "'pots' is not a mode"));

	/* The enumeration the clients walk agrees with the GType. */
	for (i = 0; i < clawt_trigger_mode_count(); i++) {
		gint value = -1;

		g_assert_true(clawt_enum_from_nick(CLAWT_TYPE_TRIGGER_MODE,
		                                   clawt_trigger_mode_nth_nick(i),
		                                   &value));
		g_assert_cmpint(value, ==, (gint)clawt_trigger_mode_nth(i));
		g_assert_cmpstr(clawt_trigger_mode_nth_hint(i), !=, "");
	}
}

/* ── The post itself ─────────────────────────────────────────────── */

static void
test_post_rendering_is_fenced_and_bounded(void)
{
	g_autoptr(ClawtConfig) config = load(
		"triggers:\n  - id: deploys\n    agent: a\n    mode: post\n"
		"    room: ops\n    instructions: 'Deployed {{repo}}'\n");
	ClawtTrigger *trigger = clawt_config_get_trigger(config, "deploys");
	g_autoptr(ClawtTriggerEvent) event = clawt_trigger_event_new(
		CLAWT_TRIGGER_PROVIDER_GENERIC, "deploy", "d1");
	g_autofree gchar *body = NULL;
	g_autofree gchar *big_body = NULL;
	g_autofree gchar *huge = NULL;
	GString *big;
	guint i;

	clawt_trigger_event_set_repo(event, "zach/clawtilla");
	clawt_trigger_event_set_payload(event, "{\"ok\":true}");
	body = clawt_trigger_render_post(trigger, event);

	g_assert_true(g_str_has_prefix(body, "Deployed zach/clawtilla\n"));
	g_assert_nonnull(strstr(body, "No agent was run"));
	g_assert_nonnull(strstr(body, "`````````untrusted-event-payload\n"));
	g_assert_nonnull(strstr(body, "\"ok\" : true"));

	/* A megabyte of payload is not a room message. */
	big = g_string_new("{\"blob\": \"");

	for (i = 0; i < 20000; i++)
		g_string_append(big, "\303\251");

	g_string_append(big, "\"}");
	huge = g_string_free(big, FALSE);
	clawt_trigger_event_set_payload(event, huge);
	big_body = clawt_trigger_render_post(trigger, event);

	g_assert_cmpuint(strlen(big_body), <, 9 * 1024);
	g_assert_true(g_utf8_validate(big_body, -1, NULL));
	g_assert_nonnull(strstr(big_body, "more bytes"));
}

/* ── The pending cap ─────────────────────────────────────────────── */

static void
test_max_pending_range_and_default(void)
{
	g_autoptr(ClawtConfig) config = load(
		"triggers:\n"
		"  - id: unset\n    agent: a\n    instructions: x\n"
		"  - id: seven\n    agent: a\n    instructions: x\n"
		"    max_pending: 7\n"
		"  - id: zero\n    agent: a\n    instructions: x\n"
		"    max_pending: 0\n"
		"  - id: huge\n    agent: a\n    instructions: x\n"
		"    max_pending: 51\n"
		"  - id: words\n    agent: a\n    instructions: x\n"
		"    max_pending: lots\n");
	const ClawtSchemaEntry *entry =
		clawt_config_schema_lookup("triggers.max_pending");
	guint fallback = (guint)g_ascii_strtoull(entry->default_value, NULL, 10);

	g_assert_cmpuint(fallback, ==, 4);
	g_assert_cmpuint(clawt_trigger_get_max_pending(
		clawt_config_get_trigger(config, "unset")), ==, fallback);
	g_assert_cmpuint(clawt_trigger_get_max_pending(
		clawt_config_get_trigger(config, "seven")), ==, 7);

	/* Out of range reads as the default, with a warning naming it. */
	g_assert_cmpuint(clawt_trigger_get_max_pending(
		clawt_config_get_trigger(config, "zero")), ==, fallback);
	g_assert_cmpuint(clawt_trigger_get_max_pending(
		clawt_config_get_trigger(config, "huge")), ==, fallback);
	g_assert_cmpuint(clawt_trigger_get_max_pending(
		clawt_config_get_trigger(config, "words")), ==, fallback);
	g_assert_true(warned(config, "trigger 'zero': max_pending 0"));
	g_assert_true(warned(config, "trigger 'huge': max_pending 51"));
	g_assert_false(warned(config, "trigger 'seven'"));

	/* A soft problem, not a broken trigger: it still validates. */
	g_assert_true(clawt_config_validate(config, NULL));
}

/* The cap reaches the batch claim: one slot, so the second is refused. */
static const gchar *
run_one(const gchar *id, const gchar *agent, const gchar *prompt,
        gpointer user_data, GError **error)
{
	guint *calls = user_data;

	(*calls)++;

	return agent;
}

static void
test_dispatch_honours_max_pending(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-match-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "triggers.db", NULL);
	g_autoptr(ClawtConfig) config = load(
		"triggers:\n  - id: ci\n    agent: builder\n    instructions: x\n"
		"    max_pending: 1\n");
	ClawtTrigger *trigger = clawt_config_get_trigger(config, "ci");
	g_autoptr(ClawtTriggerStore) store = NULL;
	g_autoptr(ClawtTriggerEvent) first = clawt_trigger_event_new(
		CLAWT_TRIGGER_PROVIDER_GENERIC, "push", "one");
	g_autoptr(ClawtTriggerEvent) second = clawt_trigger_event_new(
		CLAWT_TRIGGER_PROVIDER_GENERIC, "push", "two");
	g_autoptr(JsonNode) ran = NULL;
	g_autoptr(JsonNode) refused = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;
	guint calls = 0;

	store = clawt_trigger_store_new(path, &error);
	g_assert_no_error(error);

	ran = clawt_trigger_dispatch(trigger, store, first, "one", run_one,
	                             &calls, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ran);

	refused = clawt_trigger_dispatch(trigger, store, second, "two", run_one,
	                                 &calls, &error);
	g_assert_null(refused);
	g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
	g_assert_cmpuint(calls, ==, 1);

	/* Recorded as dropped without taking the id, so a retry is not a dup. */
	clawt_trigger_store_record_unkeyed(store, "ci", second,
	                                   CLAWT_DELIVERY_DROPPED, "over the cap");
	g_assert_false(clawt_trigger_store_seen_delivery(store, "ci", "two"));
	rows = clawt_trigger_store_list_deliveries(store, "ci", 10);
	g_assert_cmpstr(g_hash_table_lookup(rows->pdata[0], "outcome"), ==,
	                "dropped");
	g_assert_nonnull(strstr(g_hash_table_lookup(rows->pdata[0], "detail"),
	                        "delivery two"));
	g_assert_cmpuint(clawt_trigger_store_count_unfinished(store, "ci"), ==, 1);

	g_clear_object(&store);
	clawt_test_remove_tree(dir);
}

/* ── For an editor ───────────────────────────────────────────────── */

static void
test_explain_and_paths(void)
{
	g_autoptr(GPtrArray) filter = pairs("action", "opened",
	                                    "pull_request.body", "x", NULL);
	g_autoptr(JsonNode) explained = clawt_trigger_match_explain(filter,
	                                                            PAYLOAD);
	g_autoptr(JsonNode) listed = clawt_trigger_match_paths(PAYLOAD, 100);
	g_autoptr(JsonNode) capped = clawt_trigger_match_paths(PAYLOAD, 2);
	g_autoptr(JsonNode) nothing = clawt_trigger_match_paths("nope", 100);
	JsonArray *rows = json_node_get_array(explained);
	JsonArray *paths = json_node_get_array(listed);
	gboolean saw_label = FALSE;
	guint i;

	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	g_assert_true(json_object_get_boolean_member(
		json_array_get_object_element(rows, 0), "matched"));
	g_assert_cmpstr(json_object_get_string_member(
		json_array_get_object_element(rows, 0), "actual"), ==, "opened");
	g_assert_false(json_object_get_boolean_member(
		json_array_get_object_element(rows, 1), "found"));

	for (i = 0; i < json_array_get_length(paths); i++) {
		JsonObject *row = json_array_get_object_element(paths, i);

		/* Every path offered is one the filter would accept. */
		g_assert_true(clawt_trigger_match_check(
			json_object_get_string_member(row, "path"), "x", NULL));

		if (g_strcmp0(json_object_get_string_member(row, "path"),
		              "labels.1.name") == 0)
			saw_label = TRUE;
	}

	g_assert_true(saw_label);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(capped)), ==,
	                 2);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(nothing)), ==,
	                 0);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/trigger-match/lookup", test_lookup_nested_arrays_and_missing);
	g_test_add_func("/trigger-match/contains-ignores-case", test_contains_ignores_case);
	g_test_add_func("/trigger-match/all-must-match", test_all_pairs_must_match);
	g_test_add_func("/trigger-match/bad-pairs", test_bad_paths_and_values_are_refused);
	g_test_add_func("/trigger-match/from-json", test_from_json_every_spelling_and_the_limit);
	g_test_add_func("/trigger-match/config", test_config_match_reads_clears_and_limits);
	g_test_add_func("/trigger-match/mode", test_post_needs_a_room_and_unknown_mode_warns);
	g_test_add_func("/trigger-match/post-rendering", test_post_rendering_is_fenced_and_bounded);
	g_test_add_func("/trigger-match/max-pending", test_max_pending_range_and_default);
	g_test_add_func("/trigger-match/dispatch-cap", test_dispatch_honours_max_pending);
	g_test_add_func("/trigger-match/explain-and-paths", test_explain_and_paths);

	return g_test_run();
}
