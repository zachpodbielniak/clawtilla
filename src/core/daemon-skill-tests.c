/*
 * daemon-skill-tests.c - Saved inputs a skill is checked against
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A skill changes -- edited by hand, taught, a proposal accepted -- and
 * nothing said whether it still does what it was for.  A skill may now
 * carry tests: `<skill>/tests/<name>.yaml`, each an input to hand an
 * agent that has the skill and what its answer must (`expect`) and must
 * not (`reject`) say:
 *
 *   input: "Summarise this changelog: ..."
 *   agent: scout            # optional; the run can name one instead
 *   expect: ["## Fixes"]    # each must appear, case-insensitively
 *   reject: ["TODO"]        # none may appear
 *
 * skill.test_run hands each input to the agent as a task of its own, in
 * a room of its own (`skill-test:<skill>`, so it neither inherits nor
 * leaves anything in the operator's conversation), and records the run
 * against the skill's current revision.  When the task settles its
 * result is checked and the run marked passed or failed, with why.
 * skill.tests lists a skill's tests with each one's last run.
 */

#include <yaml-glib.h>

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

/* For g_ptr_array_sort(): the array holds gchar *, handed as gchar **. */
static gint
compare_names(
	gconstpointer a,
	gconstpointer b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

typedef struct {
	gchar  *name;
	gchar  *input;
	gchar  *agent;
	gchar **expect;
	gchar **reject;
} SkillTest;

static void
skill_test_free(
	SkillTest *test
){
	g_free(test->name);
	g_free(test->input);
	g_free(test->agent);
	g_strfreev(test->expect);
	g_strfreev(test->reject);
	g_free(test);
}

/* A sequence of strings, or a single string, as a NULL-terminated list. */
static gchar **
strings_of(
	YamlMapping *mapping,
	const gchar *key
){
	YamlNode *node = yaml_mapping_get_member(mapping, key);
	g_autoptr(GStrvBuilder) out = g_strv_builder_new();

	if (node == NULL)
		return NULL;

	if (yaml_node_get_node_type(node) == YAML_NODE_SCALAR) {
		g_strv_builder_add(out, yaml_node_get_string(node));
	} else if (yaml_node_get_node_type(node) == YAML_NODE_SEQUENCE) {
		YamlSequence *sequence = yaml_node_get_sequence(node);
		guint i;

		for (i = 0; i < yaml_sequence_get_length(sequence); i++) {
			YamlNode *item = yaml_sequence_get_element(sequence, i);

			if (yaml_node_get_node_type(item) == YAML_NODE_SCALAR)
				g_strv_builder_add(out, yaml_node_get_string(item));
		}
	}

	return g_strv_builder_end(out);
}

/*
 * The tests of a skill, sorted by name.  A file that does not parse, or
 * has no input, is reported in @problems rather than silently skipped.
 */
static GPtrArray *
load_tests(
	ClawtSkill *skill,
	GPtrArray  *problems
){
	GPtrArray *tests = g_ptr_array_new_with_free_func(
		(GDestroyNotify)skill_test_free);
	g_autofree gchar *dir = NULL;
	g_autoptr(GDir) listing = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	const gchar *entry;
	guint i;

	if (clawt_skill_get_directory(skill) == NULL)
		return tests;

	dir = g_build_filename(clawt_skill_get_directory(skill), "tests", NULL);
	listing = g_dir_open(dir, 0, NULL);

	if (listing == NULL)
		return tests;

	while ((entry = g_dir_read_name(listing)) != NULL) {
		if (g_str_has_suffix(entry, ".yaml") || g_str_has_suffix(entry,
		                                                         ".yml"))
			g_ptr_array_add(names, g_strdup(entry));
	}

	g_ptr_array_sort(names, compare_names);

	for (i = 0; i < names->len; i++) {
		const gchar *file_name = g_ptr_array_index(names, i);
		g_autofree gchar *path = g_build_filename(dir, file_name, NULL);
		g_autofree gchar *text = NULL;
		g_autoptr(YamlParser) parser = yaml_parser_new();
		g_autoptr(GError) error = NULL;
		YamlNode *root;
		YamlMapping *mapping;
		SkillTest *test;
		const gchar *input;

		if (!g_file_get_contents(path, &text, NULL, &error) ||
		    !yaml_parser_load_from_data(parser, text, -1, &error)) {
			g_ptr_array_add(problems, g_strdup_printf("%s: %s", file_name,
			                                          error->message));
			continue;
		}

		root = yaml_parser_get_root(parser);

		if (root == NULL || yaml_node_get_node_type(root) !=
		                    YAML_NODE_MAPPING) {
			g_ptr_array_add(problems, g_strdup_printf(
				"%s: not a mapping of input, expect and reject",
				file_name));
			continue;
		}

		mapping = yaml_node_get_mapping(root);
		input = yaml_mapping_get_string_member(mapping, "input");

		if (input == NULL || *input == '\0') {
			g_ptr_array_add(problems, g_strdup_printf("%s: has no input",
			                                          file_name));
			continue;
		}

		test = g_new0(SkillTest, 1);
		test->name = g_strndup(file_name, strrchr(file_name, '.') -
		                                  file_name);
		test->input = g_strdup(input);
		test->agent = g_strdup(yaml_mapping_get_string_member(mapping,
		                                                      "agent"));
		test->expect = strings_of(mapping, "expect");
		test->reject = strings_of(mapping, "reject");
		g_ptr_array_add(tests, test);
	}

	return tests;
}

/* Whether @needle is in @haystack, ignoring case. */
static gboolean
says(
	const gchar *haystack,
	const gchar *needle
){
	g_autofree gchar *a = g_utf8_casefold(haystack != NULL ? haystack : "",
	                                      -1);
	g_autofree gchar *b = g_utf8_casefold(needle, -1);

	return strstr(a, b) != NULL;
}

/*
 * A test's verdict on a result: every `expect` said, no `reject` said.
 * Returns the reason it failed, or NULL when it passed.
 */
gchar *
clawt_daemon_skill_test_verdict(
	const gchar         *result,
	const gchar * const *expect,
	const gchar * const *reject
){
	guint i;

	for (i = 0; expect != NULL && expect[i] != NULL; i++) {
		if (!says(result, expect[i]))
			return g_strdup_printf("the answer does not say \"%s\"",
			                       expect[i]);
	}

	for (i = 0; reject != NULL && reject[i] != NULL; i++) {
		if (says(result, reject[i]))
			return g_strdup_printf("the answer says \"%s\", which it must "
			                       "not", reject[i]);
	}

	return NULL;
}

/* A task that ended: if it is a skill test's, check it and record why. */
void
clawt_daemon_skill_tests_settled(
	ClawtDaemon *self,
	ClawtTask   *task
){
	g_autofree gchar *skill_name = NULL;
	g_autofree gchar *test_name = NULL;
	g_autoptr(GPtrArray) problems = NULL;
	g_autoptr(GPtrArray) tests = NULL;
	ClawtSkill *skill;
	guint i;

	g_return_if_fail(CLAWT_IS_DAEMON(self));

	if (self->revisions == NULL || task == NULL ||
	    !clawt_revisions_test_run_for_task(self->revisions,
	                                       clawt_task_get_id(task),
	                                       &skill_name, &test_name))
		return;

	if (clawt_task_get_state(task) != CLAWT_TASK_COMPLETED) {
		g_autofree gchar *why = g_strdup_printf(
			"the run ended %s: %s",
			clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
			                   clawt_task_get_state(task)),
			clawt_task_get_reason(task) != NULL
				? clawt_task_get_reason(task) : "no reason given");

		clawt_revisions_finish_test_run(self->revisions,
		                                clawt_task_get_id(task), "error",
		                                why);
		return;
	}

	/* The test as it reads now: its expectations, against the answer. */
	skill = (self->skills != NULL)
		? clawt_skill_library_lookup(self->skills, skill_name) : NULL;
	problems = g_ptr_array_new_with_free_func(g_free);
	tests = (skill != NULL) ? load_tests(skill, problems) : NULL;

	for (i = 0; tests != NULL && i < tests->len; i++) {
		SkillTest *test = g_ptr_array_index(tests, i);
		g_autofree gchar *failed = NULL;

		if (g_strcmp0(test->name, test_name) != 0)
			continue;

		failed = clawt_daemon_skill_test_verdict(
			clawt_task_get_result(task),
			(const gchar * const *)test->expect,
			(const gchar * const *)test->reject);
		clawt_revisions_finish_test_run(self->revisions,
		                                clawt_task_get_id(task),
		                                failed == NULL ? "passed" : "failed",
		                                failed != NULL ? failed
		                                               : "every expectation "
		                                                 "was met");
		return;
	}

	clawt_revisions_finish_test_run(self->revisions, clawt_task_get_id(task),
	                                "error",
	                                "the test was removed while it ran");
}

/* Hands one test's input to @agent_id, as a task in the test room. */
static gchar *
run_one(
	ClawtDaemon  *self,
	ClawtSkill   *skill,
	SkillTest    *test,
	const gchar  *agent_id,
	GError      **error
){
	const gchar *name = clawt_skill_get_name(skill);
	ClawtRoom *room;
	ClawtTask *task;
	g_autofree gchar *body = NULL;
	gint64 revision;

	room = clawt_room_manager_get_skill_test(self->rooms, name, agent_id);
	task = clawt_task_manager_create(self->tasks, "skill-test", agent_id,
	                                 test->input, NULL, error);

	if (task == NULL)
		return NULL;

	{
		g_autofree gchar *title = g_strdup_printf("Test %s of skill %s",
		                                          test->name, name);

		clawt_task_set_title(task, title);
		clawt_task_manager_save(self->tasks, task, NULL);
	}

	revision = clawt_revisions_newest_id(self->revisions, "skill", name);
	clawt_revisions_start_test_run(self->revisions, name, test->name,
	                               revision, agent_id,
	                               clawt_task_get_id(task));
	clawt_task_manager_start(self->tasks, clawt_task_get_id(task));

	/*
	 * Said to the agent: what this is, so it answers rather than asks.
	 * The skill is named, since using it is the point of the test.
	 */
	body = g_strdup_printf(
		"%s\n\n[clawtilla] This is test %s of your skill %s. Use the skill, "
		"answer as you would for real, and finish your turn: your final "
		"message is what is checked.", test->input, test->name, name);

	if (clawt_mailbox_router_send_to(self->router, "skill-test",
	                                 clawt_room_get_id(room), body,
	                                 clawt_task_get_id(task), 0, error) < 0) {
		clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
		                        (error != NULL && *error != NULL)
		                            ? (*error)->message
		                            : "it could not be delivered");
		return NULL;
	}

	return g_strdup(clawt_task_get_id(task));
}

/* Whether @agent_id has @skill in effect, by the one resolver. */
static gboolean
agent_has_skill(
	ClawtDaemon *self,
	const gchar *agent_id,
	const gchar *skill
){
	ClawtAgentConfig *config = clawt_config_get_agent(self->config, agent_id);
	g_autoptr(GPtrArray) bindings = NULL;
	guint i;

	if (config == NULL)
		return FALSE;

	bindings = clawt_skill_resolve_for_agent(self->config, config,
	                                         self->skills);

	for (i = 0; bindings != NULL && i < bindings->len; i++) {
		ClawtSkillBinding *binding = g_ptr_array_index(bindings, i);

		if (g_strcmp0(clawt_skill_binding_get_name(binding), skill) == 0 &&
		    clawt_skill_binding_is_active(binding))
			return TRUE;
	}

	return FALSE;
}

JsonNode *
clawt_daemon_handle_skill_tests(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GPtrArray) problems = NULL;
	g_autoptr(GPtrArray) tests = NULL;
	const gchar *name;
	ClawtSkill *skill;
	guint i;

	if (g_strcmp0(kind, "skill.tests") != 0 &&
	    g_strcmp0(kind, "skill.test_run") != 0) {
		*handled = FALSE;
		return NULL;
	}

	*handled = TRUE;
	name = clawt_ipc_payload_string(payload, "skill");
	skill = (self->skills != NULL && name != NULL)
		? clawt_skill_library_lookup(self->skills, name) : NULL;

	if (skill == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
		                           "no such skill");

	if (self->revisions == NULL)
		return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
		                           "test runs are kept in revisions.db, "
		                           "which could not be opened");

	problems = g_ptr_array_new_with_free_func(g_free);
	tests = load_tests(skill, problems);
	builder = json_builder_new();

	if (g_strcmp0(kind, "skill.tests") == 0) {
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "tests");
		json_builder_begin_array(builder);

		for (i = 0; i < tests->len; i++) {
			SkillTest *test = g_ptr_array_index(tests, i);
			JsonNode *last = clawt_revisions_last_test_run(self->revisions,
			                                               name, test->name);

			json_builder_begin_object(builder);
			clawt_daemon_add_string_member(builder, "name", test->name);
			clawt_daemon_add_string_member(builder, "input", test->input);
			clawt_daemon_add_string_member(builder, "agent", test->agent);
			if (last != NULL) {
				json_builder_set_member_name(builder, "last");
				json_builder_add_value(builder, last);
			}
			json_builder_end_object(builder);
		}

		json_builder_end_array(builder);
		json_builder_set_member_name(builder, "problems");
		json_builder_begin_array(builder);
		for (i = 0; i < problems->len; i++)
			json_builder_add_string_value(builder,
			                              g_ptr_array_index(problems, i));
		json_builder_end_array(builder);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	/* skill.test_run: one named test, or every one. */
	{
		const gchar *only = clawt_ipc_payload_string(payload, "test");
		const gchar *agent = clawt_ipc_payload_string(payload, "agent");
		guint started = 0;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "runs");
		json_builder_begin_array(builder);

		for (i = 0; i < tests->len; i++) {
			SkillTest *test = g_ptr_array_index(tests, i);
			const gchar *who = (agent != NULL) ? agent : test->agent;
			g_autoptr(GError) error = NULL;
			g_autofree gchar *task_id = NULL;

			if (only != NULL && g_strcmp0(only, test->name) != 0)
				continue;

			if (who == NULL)
				return clawt_ipc_error_new(request,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "name the agent to run it (`agent`), or put one in the "
				    "test file");

			if (!agent_has_skill(self, who, name))
				return clawt_ipc_error_new(request,
				    CLAWT_ERROR_INVALID_ARGUMENT,
				    "that agent does not have this skill in effect, so the "
				    "test would test nothing; give it the skill first");

			task_id = run_one(self, skill, test, who, &error);

			if (task_id == NULL)
				return clawt_ipc_error_new(request, error->code,
				                           error->message);

			json_builder_begin_object(builder);
			clawt_daemon_add_string_member(builder, "test", test->name);
			clawt_daemon_add_string_member(builder, "task", task_id);
			clawt_daemon_add_string_member(builder, "agent", who);
			json_builder_end_object(builder);
			started++;
		}

		json_builder_end_array(builder);
		json_builder_end_object(builder);

		if (started == 0)
			return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
			                           only != NULL
			                               ? "no such test"
			                               : "this skill has no tests: add "
			                                 "tests/<name>.yaml beside its "
			                                 "SKILL.md");

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}
}
